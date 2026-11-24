// ============================================================================
//  MHW RNG Official Plugin: Endemic Life Size, Crown & Rare Controller (`rng_endemic.dll`)
//  Version : 1.3.0 (SDK v3.4)
//  Targets :
//    [Size & Crown]
//    1. `RVA 0x166CFE1` (`FUN_14166ccc0` @ 0x14166CCC0)
//       Crown Tier Roll (`0 = Mini Crown`, `1 = Normal`, `2 = Giant Crown`),
//       shared by both field spawns (`0x141D2A1E0 -> 0x140271840`) and
//       capture-time species rolls (`0x141D28C00 -> 0x141669AC0`).
//    2. `RVA 0x1D2A472` / `RVA 0x1D2A3CC` (`FUN_141d2a1e0` calling `FUN_140672f90`)
//       Field-spawn intra-tier linear size interpolation (directly unwrapped from
//       the microkernel's built-in `0x0672FBC` transparent wrapper so non-endemic
//       callers of `FUN_140672f90` never enter this plugin).
//    3. `RVA 0x1669CB6` (`0x141669AC0`)
//       Capture-time intra-tier linear size interpolation for species whose
//       size is determined on capture (`sizeMode == 3`).
//    [Rare Endemic Life / Great-Size Fish / Guiding Lands Moly Spawn (Pure-RNG)]
//    4. `RVA 0x1A4ACED` (`FUN_141a4a9b0` @ 0x141A4A9B0)
//       Master rare endemic, rare fish, extra Great-Size fish & Guiding Lands
//       Moly burrow activation roll (on map load init and each in-game hour).
//    5. `RVA 0x0271F25` (`0x140271ce0` @ 0x140271CE0)
//       Guiding Lands Rare Regional Moly (`state = 7`) vs Normal (`state = 8`).
//    6. `RVA 0x166D12E` (`FUN_14166d070` @ 0x14166D070, caller `0x140271f90`)
//       Group-level rare availability flag (`cAnimalGroupCtrl + 0x334`).
//    7. `RVA 0x1A44FCC` (`FUN_141a44f50` @ 0x141A44F50)
//       Mutually-exclusive rare spawn group tie-breaker (protects forced rare
//       slots from being wiped by inactive group siblings).
//
//  Architecture & SDK Best-Practice Highlights:
//    - Zero extra game hooks / code patches: operates purely via `RegisterSlotHookEx`.
//    - Respects vanilla weather, time-of-day & Guiding Lands region-level gates
//      while letting enabled rare species roll at 100% (or 0% blocked) cleanly.
//    - Reads live `SizeSetData::mSizeProbability[3]` + active Felyne food /
//      Bounty bonuses from `0x14166CCC0`'s stack frame, computing exact legal
//      midpoints so disabled 0-probability crown tiers are skipped cleanly and
//      Felyne food buffs never skew targeted tiers.
//    - Reads live `rAnimalSize` (`exeBase + 0x5012740`) for exact `cm`/scale
//      solving (`$rng endemic exact ...`) and live spawn/capture telemetry.
//    - Strict leaf-mutex discipline: `g_stateMx` is never held while invoking
//      `g_host->...`.
// ============================================================================
#include "mhw_rng_api.h"
#include "mhw_rng_math.h"
#include "mhw_1523_world.h"
#include "mhw_world_api.h"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

// Verified MHW:I 15.23 (Steam 15539686) RVAs & Offsets:
constexpr std::uint32_t  kRvaCrownTierRoll         = 0x166CFE1U; // `FUN_14166ccc0` `CALL MtRandom::nrand`
constexpr std::uint32_t  kRvaFieldRangeStd         = 0x1D2A472U; // `0x141D2A1E0` -> `CALL 0x140672f90` (unwrapped via 0x0672FBC)
constexpr std::uint32_t  kRvaFieldRangeSpec        = 0x1D2A3CCU; // `0x141D2A1E0` -> `CALL 0x140672f90` (special region spawn)
constexpr std::uint32_t  kRvaCaptureIntraTierRoll  = 0x1669CB6U; // `0x141669AC0` `CALL MtRandom::nrand`
constexpr std::uint32_t  kRvaRareMasterRoll        = 0x1A4ACEDU; // `FUN_141a4a9b0` `CALL MtRandom::nrand`
constexpr std::uint32_t  kRvaRareMolyRoll          = 0x0271F25U; // `0x140271ce0` `CALL MtRandom::nrand`
constexpr std::uint32_t  kRvaRareGroupRoll         = 0x166D12EU; // `FUN_14166d070` `CALL MtRandom::nrand`
constexpr std::uint32_t  kRvaRareMutexTieBreak     = 0x1A44FCCU; // `FUN_141a44f50` `CALL MtRandom::nrand`

// Verified caller return RVAs for stack-frame validation:
constexpr std::uintptr_t kRvaCallerCaptureRollRet  = 0x1669C19U; // `0x141669AC0` -> `CALL 0x14166ccc0`
constexpr std::uintptr_t kRvaCallerFieldWrapLo     = 0x0271840U; // `0x140271840` (`nAnimal::SizeSetData::ctor`) start
constexpr std::uintptr_t kRvaCallerFieldWrapHi     = 0x0271960U; // `0x140271840` end
constexpr std::uintptr_t kRvaCallerFieldTailRet    = 0x1D2A407U; // `0x141D2A1E0` -> `CALL 0x140271840` (tail-jmp to 0x14166CCC0)
constexpr std::uintptr_t kRvaCallerGroupRareRet    = 0x0271FFFU; // `0x140271f90` -> `CALL 0x14166d070`

// Verified `rAnimalSize`, `uAnimal`, `sAnimal` & `cAnimalGroupCtrl` offsets (from `mhw_1523_world.h`):
constexpr std::uintptr_t kRvaAnimalSizeGlobalPtr   = mhw1523::kRvaRAnimalSize;
constexpr std::uintptr_t kOffUAnimalId             = mhw1523::kOffUAnimalId;
constexpr std::uintptr_t kOffUAnimalType           = mhw1523::kOffUAnimalType;
constexpr std::uintptr_t kOffSAnimalRareSlotBase   = mhw1523::kOffSAnimalRareSlotBase;
constexpr std::uintptr_t kRareSlotStride           = mhw1523::kRareSlotStride;
constexpr std::uintptr_t kOffRareSlotJustRolled    = mhw1523::kOffRareSlotJustRolled;
constexpr std::uintptr_t kOffGroupCtrlAnimalId     = mhw1523::kOffGroupCtrlAnimalId;
constexpr std::uintptr_t kOffGroupCtrlAnimalType   = mhw1523::kOffGroupCtrlAnimalType;
constexpr std::uintptr_t kOffGroupCtrlRegionId     = mhw1523::kOffGroupCtrlRegionId;

constexpr float          kProbEps                  = mhw_sdk::kProbEpsilon;
using mhw_sdk::kMinSafeFloatU32;
using mhw_sdk::kMaxSafeFloatU32;
using mhw_sdk::IsValidUserPtr;
using mhw_sdk::FractionToTierU32;
using mhw_sdk::FractionToOpenSafeU32;
constexpr std::uint32_t  kRareForceU32             = 0x00000001U; // 2^-32: passes any prob > 0 while skipping 0.0f disabled entries
constexpr std::uint32_t  kRareBlockU32             = 0xFFFFFFFFU; // 1.0f : blocks any prob < 1.0f cleanly
constexpr std::uint32_t  kAnyAnimalType            = 0xFFFFFFFFU;
constexpr std::uint32_t  kMaxRareMutexSlots        = 128U;

enum class PolicyMode : std::uint8_t {
    kNone       = 0, // Unmodified (MasterDerived / kernel default)
    kFixedTier  = 1, // Lock crown tier (0=Mini, 1=Normal, 2=Giant) + intra-tier ratio [0..100]
    kExactScale = 2, // Lock exact multiplier (e.g. 1.18x) via live `rAnimalSize`
    kExactCm    = 3  // Lock exact size in cm (e.g. 24.5cm) via live `rAnimalSize`
};

struct EndemicPolicy {
    PolicyMode    mode         = PolicyMode::kNone;
    std::uint32_t animalId     = 0U;              // 0 = global
    std::uint32_t animalType   = kAnyAnimalType;  // kAnyAnimalType = all variants of `animalId`
    std::uint32_t condId       = 0U;              // Composite condition id for kernel telemetry
    std::uint32_t targetTier   = 1U;              // 0 = Mini, 1 = Normal, 2 = Giant
    double        intraPct     = 50.0;            // [0.0 .. 100.0] position inside target tier
    float         exactValue   = 1.0f;            // Target scale or target cm
};

struct AnimalSizeRow {
    bool         valid       = false;
    std::int32_t sizeMode    = 0;
    float        baseSizeCm  = 0.0f;
    float        minScale    = 1.0f;
    float        maxScale    = 1.0f;
    float        tierMin[3]{1.0f, 1.0f, 1.0f};
    float        tierMax[3]{1.0f, 1.0f, 1.0f};
};

struct EndemicHistoryEntry {
    bool          validSpecies  = false;
    bool          validRow      = false;
    bool          isCapturePath = false;
    bool          hasRolledSize = false;
    std::uint32_t animalId      = 0U;
    std::uint32_t animalType    = 0U;
    std::uint32_t rolledTier    = 1U; // 0 = Mini, 1 = Normal, 2 = Giant
    float         baseSizeCm    = 0.0f;
    float         minScale      = 1.0f;
    float         maxScale      = 1.0f;
    float         rolledScale   = 1.0f;
};

struct ThreadLocalEndemicCtx {
    bool          valid         = false;
    bool          isCapturePath = false;
    bool          hasExactIntra = false;
    bool          wasLocked     = false;
    bool          validProbs    = false;
    std::uint32_t animalId      = 0U;
    std::uint32_t animalType    = 0U;
    std::uint32_t chosenTier    = 1U;
    std::uint32_t exactIntraU32 = 0x80000000U;
    float         effProb[3]{};
    float         totalBase     = 0.0f;
    AnimalSizeRow sizeRow{};
};

enum class RareMode : std::uint8_t {
    kVanilla  = 0,
    kForceOn  = 1,
    kForceOff = 2
};

struct RarePolicy {
    RareMode      mode       = RareMode::kVanilla;
    std::uint32_t animalId   = 0U;
    std::uint32_t animalType = kAnyAnimalType;
    std::uint32_t condId     = 0U;
};

struct RareHistoryEntry {
    std::uint8_t  siteKind    = 0U; // 0=master, 1=moly, 2=group, 3=mutex
    bool          validSp     = false;
    bool          spawned     = false;
    bool          overridden  = false;
    bool          isInit      = true;
    std::uint32_t animalId    = 0U;
    std::uint32_t animalType  = 0U;
    std::int32_t  extra       = 0;  // master/group: targetWeather; moly: regionIdx; mutex: winSlotPos
    float         effProb     = -1.0f;
    std::uint8_t  startHour   = 0U;
    std::uint8_t  endHour     = 24U;
};

struct RareTlsRollCtx {
    bool          valid      = false;
    bool          overridden = false;
    bool          isInit     = true;
    std::uint8_t  siteKind   = 0U;
    std::uint32_t animalId   = 0U;
    std::uint32_t animalType = 0U;
    std::int32_t  extra      = 0;
    float         effProb    = -1.0f;
    std::uint8_t  startHour  = 0U;
    std::uint8_t  endHour    = 24U;
};

struct RareMutexCand {
    std::uint32_t k          = 0U;
    bool          validSp    = false;
    std::uint32_t animalId   = 0U;
    std::uint32_t animalType = 0U;
};

struct RareMutexTlsCtx {
    bool          valid      = false;
    bool          overridden = false;
    std::uint32_t groupCount = 0U;
    std::uint32_t winnerK    = 0U;
    std::size_t   candCount  = 0U;
    RareMutexCand cands[kMaxRareMutexSlots]{};
};

constexpr std::size_t kHistoryCap     = 6U;
constexpr std::size_t kRareHistoryCap = 12U;

const MhwRngHostApi*       g_host   = nullptr;
MhwRngPluginId             g_selfId = 0U;

// Strict leaf mutex: NEVER call `g_host->...` while holding `g_stateMx`!
std::mutex                 g_stateMx;
EndemicPolicy              g_globalPolicy{};
std::vector<EndemicPolicy> g_speciesPolicies;
EndemicHistoryEntry        g_history[kHistoryCap]{};
std::size_t                g_historyCount = 0U;

RarePolicy                 g_globalRarePolicy{};
std::vector<RarePolicy>    g_speciesRarePolicies;
bool                       g_rareLogEnabled   = false;
RareHistoryEntry           g_rareHistory[kRareHistoryCap]{};
std::size_t                g_rareHistoryCount = 0U;
MhwRngHookHandle           g_worldEvHandle    = kMhwRngInvalidHandle;

thread_local ThreadLocalEndemicCtx tls_lastRoll{};
thread_local RareTlsRollCtx        tls_lastRareRoll{};
thread_local RareMutexTlsCtx       tls_lastRareMutex{};

inline bool IsEn() noexcept {
    return MhwRng_HostIsEnglish(g_host);
}

// ============================================================================
//  String, Formatting & Selector Helpers
// ============================================================================

std::string LowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

// `IsValidUserPtr` is imported from `mhw_rng_math.h`.

std::uint32_t MakeSpeciesCondId(std::uint32_t animalId, std::uint32_t animalType) noexcept {
    if (animalId == 0U) return 0U;
    if (animalType == kAnyAnimalType) {
        return (animalId << 8) | 0xFFU;
    }
    return (animalId << 8) | (animalType & 0x7FU);
}

std::string FormatSpeciesCodeRaw(std::uint32_t animalId, std::uint32_t animalType) {
    char buf[32]{};
    if (animalType == kAnyAnimalType) {
        std::snprintf(buf, sizeof(buf), "ec%03u*", animalId);
    } else {
        std::snprintf(buf, sizeof(buf), "ec%03u_%02u", animalId, animalType);
    }
    return buf;
}

// ----------------------------------------------------------------------------
//  Endemic life name table (Simplified Chinese / English), auto-generated from
//  the game's own research-note + item text data (see `endemic_names.inl`).
// ----------------------------------------------------------------------------
struct EndemicNameEntry {
    std::uint16_t animalId;
    std::uint16_t animalType;
    std::uint8_t  isRare;   // 1 = research-note rare flag, 0 = common, 2 = gimmick creature
    std::uint8_t  hasGreat; // 1 = fish with a separate Great-size item
    const char*   zh;
    const char*   en;
    const char*   slug;
};

#define ENDEMIC_NAME(id, tp, rare, great, zh, en, sl) \
    { static_cast<std::uint16_t>(id), static_cast<std::uint16_t>(tp), \
      static_cast<std::uint8_t>(rare), static_cast<std::uint8_t>(great), zh, en, sl },
const EndemicNameEntry kEndemicNames[] = {
#include "endemic_names.inl"
};
#undef ENDEMIC_NAME

const EndemicNameEntry* FindEndemicName(std::uint32_t animalId, std::uint32_t animalType) noexcept {
    for (const auto& e : kEndemicNames) {
        if (e.animalId == animalId && e.animalType == animalType) return &e;
    }
    return nullptr;
}

std::size_t CountEndemicVariants(std::uint32_t animalId) noexcept {
    std::size_t n = 0U;
    for (const auto& e : kEndemicNames) {
        if (e.animalId == animalId) ++n;
    }
    return n;
}

// Human-readable label, e.g. `ec058_00(月羽天母)` / `ec059*(Moly, all variants)`.
std::string FormatSpeciesCode(std::uint32_t animalId, std::uint32_t animalType) {
    std::string code = FormatSpeciesCodeRaw(animalId, animalType);
    const bool en = IsEn();
    if (animalType == kAnyAnimalType) {
        const EndemicNameEntry* base = FindEndemicName(animalId, 0U);
        if (!base) return code;
        const bool multi = CountEndemicVariants(animalId) > 1U;
        code += "(";
        code += en ? base->en : base->zh;
        if (multi) code += en ? ", all variants" : u8" 全变种";
        code += ")";
        return code;
    }
    const EndemicNameEntry* e = FindEndemicName(animalId, animalType);
    if (!e) return code;
    code += "(";
    code += en ? e->en : e->zh;
    code += ")";
    return code;
}

const char* TierName(std::uint32_t tier, bool en) noexcept {
    switch (tier) {
        case 0U: return en ? "Mini"   : u8"小金";
        case 2U: return en ? "Giant"  : u8"大金";
        default: return en ? "Normal" : u8"普通";
    }
}

std::string FormatPolicySummary(const EndemicPolicy& p, bool en) {
    char buf[80]{};
    switch (p.mode) {
        case PolicyMode::kFixedTier:
            if (en) {
                std::snprintf(buf, sizeof(buf), "%s crown (intra %.0f%%)", TierName(p.targetTier, true), p.intraPct);
            } else {
                std::snprintf(buf, sizeof(buf), u8"%s档(档内%.0f%%)", TierName(p.targetTier, false), p.intraPct);
            }
            return buf;
        case PolicyMode::kExactScale:
            std::snprintf(buf, sizeof(buf), en ? "Exact scale=%.2fx" : u8"精确倍率=%.2fx", p.exactValue);
            return buf;
        case PolicyMode::kExactCm:
            std::snprintf(buf, sizeof(buf), en ? "Exact size=%.2fcm" : u8"精确查表尺寸=%.2fcm", p.exactValue);
            return buf;
        default:
            return en ? "Unlocked (vanilla)" : u8"未锁定(正常随机)";
    }
}

const char* FormatRareModeSummary(RareMode m, bool en) noexcept {
    switch (m) {
        case RareMode::kForceOn:  return en ? "Force 100% (Pure-RNG)" : u8"强制必刷(100% 纯RNG)";
        case RareMode::kForceOff: return en ? "Blocked (0%)"          : u8"屏蔽刷新(0%)";
        default:                  return en ? "Vanilla (unlocked)"    : u8"未锁定(正常随机)";
    }
}

const char* RareSiteTag(std::uint8_t siteKind, bool en) noexcept {
    switch (siteKind) {
        case 1U: return en ? "Moly"  : u8"地区稀有毛吉";
        case 2U: return en ? "Group" : u8"族群稀有";
        case 3U: return en ? "Mutex" : u8"互斥保底";
        default: return en ? "Rare"  : u8"稀有生成";
    }
}

const char* WeatherGateTag(std::int32_t weatherGate, bool en) noexcept {
    switch (weatherGate) {
        case 1:  return en ? "Clear"   : u8"晴天";
        case 2:  return en ? "Special" : u8"特殊天气";
        default: return en ? "Any"     : u8"不限天气";
    }
}

// Parses species selectors:
//   - `ec059` / `ec59` / `#59`           -> (animalId=59, animalType=kAnyAnimalType)
//   - `ec059_02` / `ec59_2` / `59:2`     -> (animalId=59, animalType=2)
//   - `掘土毛吉` / `rocky_moly`           -> exact variant from the game-data name table
//   - `毛吉*` / `moly*`                   -> whole family of that variant's animalId
bool ParseSpeciesSelector(const std::string& rawTok,
                          std::uint32_t& outAnimalId,
                          std::uint32_t& outAnimalType) {
    if (rawTok.empty()) return false;
    const std::string low = LowerAscii(rawTok);

    {
        std::string nameTok = rawTok;
        std::string nameLow = low;
        bool family = false;
        if (nameTok.size() > 1U && nameTok.back() == '*') {
            family = true;
            nameTok.pop_back();
            nameLow.pop_back();
        }
        for (const auto& e : kEndemicNames) {
            if (nameTok == e.zh || nameLow == e.slug) {
                outAnimalId   = e.animalId;
                outAnimalType = family ? kAnyAnimalType : e.animalType;
                return true;
            }
        }
    }

    const char* s = low.c_str();
    if (low.rfind("ec", 0) == 0 && low.size() >= 3U) {
        s += 2;
    } else if (low[0] == '#' && low.size() >= 2U) {
        s += 1;
    } else if (low.find(':') != std::string::npos || low.find('_') != std::string::npos) {
        if (!std::isdigit(static_cast<unsigned char>(low[0]))) return false;
    } else {
        return false; // Plain integers without `ec`/`#`/`:` belong to numeric ratio arguments
    }

    char* ep = nullptr;
    const unsigned long id = std::strtoul(s, &ep, 10);
    if (ep == s || id == 0UL || id > 999UL) return false;

    if (*ep == '\0' || (*ep == '*' && ep[1] == '\0')) {
        outAnimalId   = static_cast<std::uint32_t>(id);
        outAnimalType = kAnyAnimalType;
        return true;
    }
    if (*ep == '_' || *ep == ':') {
        ++ep;
        if (*ep == '*' && ep[1] == '\0') {
            outAnimalId   = static_cast<std::uint32_t>(id);
            outAnimalType = kAnyAnimalType;
            return true;
        }
        char* ep2 = nullptr;
        const unsigned long tp = std::strtoul(ep, &ep2, 10);
        if (ep2 == ep || *ep2 != '\0' || tp > 127UL) return false;
        outAnimalId   = static_cast<std::uint32_t>(id);
        outAnimalType = static_cast<std::uint32_t>(tp);
        return true;
    }
    return false;
}

// `FractionToTierU32` & `FractionToOpenSafeU32` are imported from `mhw_rng_math.h`.

// ============================================================================
//  SEH-Safe Live Reading of `rAnimalSize` (`0x145012740`) & Stack Context
// ============================================================================

bool ReadAnimalSizeRowSEH(std::uintptr_t exeBase,
                          std::uint32_t animalId,
                          std::uint32_t animalType,
                          bool isCapturePath,
                          AnimalSizeRow& out) noexcept {
    if (!exeBase) return false;
    __try {
        const std::uintptr_t mgr = *reinterpret_cast<const std::uintptr_t*>(exeBase + kRvaAnimalSizeGlobalPtr);
        if (!IsValidUserPtr(mgr)) return false;
        const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(mgr + 0xB8U);
        const std::uintptr_t arr  = *reinterpret_cast<const std::uintptr_t*>(mgr + 0xC8U);
        if (count == 0U || count > 4096U || !IsValidUserPtr(arr)) return false;

        const auto* entries = reinterpret_cast<const std::uintptr_t*>(arr);
        const std::uintptr_t defaultRow = entries[0];
        if (!IsValidUserPtr(defaultRow)) return false;

        for (std::uint32_t i = 0U; i < count; ++i) {
            const std::uintptr_t row = entries[i];
            if (!IsValidUserPtr(row)) continue;
            const auto rid = *reinterpret_cast<const std::int32_t*>(row + 0x08U);
            const auto rtp = *reinterpret_cast<const std::int32_t*>(row + 0x0CU);
            if (static_cast<std::uint32_t>(rid) != animalId || static_cast<std::uint32_t>(rtp) != animalType) {
                continue;
            }

            const auto sizeMode = *reinterpret_cast<const std::int32_t*>(row + 0x2CU);
            out.sizeMode   = sizeMode;
            out.baseSizeCm = *reinterpret_cast<const float*>(row + 0x24U);
            if (!(out.baseSizeCm > 0.0f) || !std::isfinite(out.baseSizeCm)) out.baseSizeCm = 0.0f;

            const float minS = *reinterpret_cast<const float*>(row + 0x30U);
            const float maxS = *reinterpret_cast<const float*>(row + 0x34U);
            const float span = maxS - minS;

            if (sizeMode == 2 || !(span > 0.0f) || (sizeMode == 3 && !isCapturePath)) {
                out.minScale = 1.0f;
                out.maxScale = 1.0f;
                for (int t = 0; t < 3; ++t) {
                    out.tierMin[t] = 1.0f;
                    out.tierMax[t] = 1.0f;
                }
                out.valid = true;
                return true;
            }

            const std::uintptr_t ratioRow = (sizeMode == 0) ? defaultRow : row;
            const float rSmallHi = *reinterpret_cast<const float*>(ratioRow + 0x38U);
            const float rNormLo  = *reinterpret_cast<const float*>(ratioRow + 0x3CU);
            const float rNormHi  = *reinterpret_cast<const float*>(ratioRow + 0x40U);
            const float rLargeLo = *reinterpret_cast<const float*>(ratioRow + 0x44U);

            out.minScale   = minS;
            out.maxScale   = maxS;
            out.tierMin[0] = minS;
            out.tierMax[0] = minS + span * rSmallHi;
            out.tierMin[1] = minS + span * rNormLo;
            out.tierMax[1] = minS + span * rNormHi;
            out.tierMin[2] = minS + span * rLargeLo;
            out.tierMax[2] = maxS;
            out.valid      = true;
            return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct LiveCrownTierContext {
    bool          validSpecies  = false;
    bool          validProbs    = false;
    bool          isCapturePath = false;
    std::uint32_t animalId      = 0U;
    std::uint32_t animalType    = 0U;
    float         baseProb[3]{};
    float         bonus[3]{};
    float         effProb[3]{};
    float         totalBase     = 0.0f;
    AnimalSizeRow sizeRow{};
};

bool ReadCrownTierContextSEH(const MhwRngDrawContext* ctx, LiveCrownTierContext& out) noexcept {
    if (!ctx || !ctx->returnAddrSlot || !ctx->exeBase) return false;
    __try {
        const auto* slotPtr = static_cast<const std::uintptr_t*>(ctx->returnAddrSlot);

        // Verified stack layout of `FUN_14166ccc0` (`0x14166CCC0`) at `CALL MtRandom::nrand` (`0x166CFE1`):
        //   7 pushes (`RBP..R15` = 0x38) + `SUB RSP, 0x80` -> `S0 = returnAddrSlot + 0xC0 = &slotPtr[24]`
        //   - `slotPtr[24]`: caller return address (`0x1669C19`, inside `0x0271840..0x0271960`, or `0x1D2A407` tail-jmp)
        //   - `slotPtr[21]`: saved `RDI` (`animalId` on direct calls, or `uAnimal*` on tail-jmp from `0x140271840`)
        //   - `slotPtr[26]`: saved `RBX` (`animalType` on direct calls)
        //   - `slotPtr[5]` : `float local_98[3]` (`bonus[0..2]` at `RSP + 0x20`)
        //   - `slotPtr[25]`: saved `RDI = SizeSetData* - &local_98[0]` (at `RSP + 0xC0`)
        const std::uintptr_t callerRet = slotPtr[24];
        if (callerRet == (ctx->exeBase + kRvaCallerCaptureRollRet)) {
            out.animalId      = static_cast<std::uint32_t>(slotPtr[21]);
            out.animalType    = static_cast<std::uint32_t>(slotPtr[26]);
            out.isCapturePath = true;
            out.validSpecies  = (out.animalId > 0U && out.animalId < 1000U && out.animalType < 256U);
        } else if (callerRet >= (ctx->exeBase + kRvaCallerFieldWrapLo) &&
                   callerRet < (ctx->exeBase + kRvaCallerFieldWrapHi)) {
            out.animalId      = static_cast<std::uint32_t>(slotPtr[21]);
            out.animalType    = static_cast<std::uint32_t>(slotPtr[26]);
            out.isCapturePath = false;
            out.validSpecies  = (out.animalId > 0U && out.animalId < 1000U && out.animalType < 256U);
        } else if (callerRet == (ctx->exeBase + kRvaCallerFieldTailRet)) {
            const std::uintptr_t uAnimal = slotPtr[21];
            if (IsValidUserPtr(uAnimal)) {
                out.animalId      = *reinterpret_cast<const std::uint32_t*>(uAnimal + kOffUAnimalId);
                out.animalType    = *reinterpret_cast<const std::uint32_t*>(uAnimal + kOffUAnimalType);
                out.isCapturePath = false;
                out.validSpecies  = (out.animalId > 0U && out.animalId < 1000U && out.animalType < 256U);
            }
        }

        // Extract live `SizeSetData::mSizeProbability[3]` + `local_98[3]` bonuses
        const auto* bonusPtr = reinterpret_cast<const float*>(slotPtr + 5);
        const auto  rdiDiff  = static_cast<std::intptr_t>(slotPtr[25]);
        const auto  sizeSet  = static_cast<std::uintptr_t>(reinterpret_cast<std::intptr_t>(bonusPtr) + rdiDiff);
        if (IsValidUserPtr(sizeSet, 0x3ULL)) {
            float sumBase = 0.0f;
            float sumEff  = 0.0f;
            bool  allFin  = true;
            for (int t = 0; t < 3; ++t) {
                const float bp = *reinterpret_cast<const float*>(sizeSet + 0x14U + static_cast<std::uintptr_t>(t) * 4U);
                const float bn = bonusPtr[t];
                if (!std::isfinite(bp) || !std::isfinite(bn) || bp < -0.01f || bp > 1000.0f) {
                    allFin = false;
                    break;
                }
                out.baseProb[t] = bp;
                out.bonus[t]    = bn;
                sumBase += bp;
                const float eff = (bp >= kProbEps && (bp + bn) > 0.0f) ? (bp + bn) : 0.0f;
                out.effProb[t]  = eff;
                sumEff += eff;
            }
            if (allFin && sumBase > kProbEps && sumEff > kProbEps) {
                out.totalBase  = sumBase;
                out.validProbs = true;
            }
        }

        if (out.validSpecies) {
            ReadAnimalSizeRowSEH(ctx->exeBase, out.animalId, out.animalType, out.isCapturePath, out.sizeRow);
        }
        return out.validSpecies || out.validProbs;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Solves the exact `uint32_t` for `FUN_14166ccc0` to select `wantTier` (or nearest legal tier with `effProb > 0`)!
std::uint32_t SolveCrownTierU32(const LiveCrownTierContext& sc,
                                std::uint32_t wantTier,
                                std::uint32_t& outChosenTier) noexcept {
    if (!sc.validProbs || !(sc.totalBase > kProbEps)) {
        outChosenTier = (wantTier <= 2U) ? wantTier : 1U;
        if (outChosenTier == 0U) return kMinSafeFloatU32;
        if (outChosenTier == 2U) return kMaxSafeFloatU32;
        return 0x80000000U;
    }

    int chosen = -1;
    if (wantTier <= 2U && sc.effProb[wantTier] > 0.0f) {
        chosen = static_cast<int>(wantTier);
    } else if (wantTier == 0U) {
        for (int t = 0; t < 3; ++t) { if (sc.effProb[t] > 0.0f) { chosen = t; break; } }
    } else if (wantTier == 2U) {
        for (int t = 2; t >= 0; --t) { if (sc.effProb[t] > 0.0f) { chosen = t; break; } }
    } else {
        for (int t = 1; t >= 0; --t) { if (sc.effProb[t] > 0.0f) { chosen = t; break; } }
        if (chosen < 0 && sc.effProb[2] > 0.0f) chosen = 2;
    }
    if (chosen < 0) {
        outChosenTier = 1U;
        return 0x80000000U;
    }
    outChosenTier = static_cast<std::uint32_t>(chosen);

    double cumBefore = 0.0;
    for (int t = 0; t < chosen; ++t) cumBefore += static_cast<double>(sc.effProb[t]);
    const double mid = cumBefore + 0.5 * static_cast<double>(sc.effProb[chosen]);
    const double frac = mid / static_cast<double>(sc.totalBase);
    return FractionToOpenSafeU32(frac);
}

std::uint32_t SimulateCrownTier(const LiveCrownTierContext& sc, std::uint32_t u32Val) noexcept {
    if (!sc.validProbs) return 1U;
    const float roll = static_cast<float>(static_cast<std::uint64_t>(u32Val)) * (1.0f / 4294967296.0f) * sc.totalBase;
    float cum = 0.0f;
    for (std::uint32_t t = 0U; t < 3U; ++t) {
        if (sc.effProb[t] <= 0.0f) continue;
        cum += sc.effProb[t];
        if (roll <= cum) return t;
    }
    return 1U;
}

// Finds active policy under `g_stateMx` (exact `ecXXX_YY` > wildcard `ecXXX*` > global).
template <typename PolicyT, typename Pred>
const PolicyT* FindActiveRuleLocked(const PolicyT& globalPol,
                                    const std::vector<PolicyT>& spList,
                                    bool validSpecies,
                                    std::uint32_t animalId,
                                    std::uint32_t animalType,
                                    Pred isActive,
                                    bool* outIsSpeciesRule = nullptr) noexcept {
    if (outIsSpeciesRule) *outIsSpeciesRule = false;
    if (validSpecies) {
        for (const auto& p : spList) {
            if (isActive(p) && p.animalId == animalId && p.animalType == animalType) {
                if (outIsSpeciesRule) *outIsSpeciesRule = true;
                return &p;
            }
        }
        for (const auto& p : spList) {
            if (isActive(p) && p.animalId == animalId && p.animalType == kAnyAnimalType) {
                if (outIsSpeciesRule) *outIsSpeciesRule = true;
                return &p;
            }
        }
    }
    return &globalPol;
}

const EndemicPolicy* FindActivePolicyLocked(bool validSpecies,
                                            std::uint32_t animalId,
                                            std::uint32_t animalType) noexcept {
    return FindActiveRuleLocked(
        g_globalPolicy, g_speciesPolicies, validSpecies, animalId, animalType,
        [](const EndemicPolicy& p) noexcept { return p.mode != PolicyMode::kNone; });
}

const RarePolicy* FindActiveRarePolicyLocked(bool validSpecies,
                                             std::uint32_t animalId,
                                             std::uint32_t animalType,
                                             bool* outIsSpeciesRule = nullptr) noexcept {
    return FindActiveRuleLocked(
        g_globalRarePolicy, g_speciesRarePolicies, validSpecies, animalId, animalType,
        [](const RarePolicy& p) noexcept { return p.mode != RareMode::kVanilla; },
        outIsSpeciesRule);
}

bool HasAnyRareForceOnLocked() noexcept {
    if (g_globalRarePolicy.mode == RareMode::kForceOn) return true;
    for (const auto& p : g_speciesRarePolicies) {
        if (p.mode == RareMode::kForceOn) return true;
    }
    return false;
}

std::uint32_t ComputeIntraTierU32(const EndemicPolicy& pol, std::uint32_t tier) noexcept {
    const double frac = std::clamp(pol.intraPct, 0.0, 100.0) / 100.0;
    // Allow exact `0xFFFFFFFFU` (`1.0f`) on Giant Crown (`tier == 2`) so floor-truncation reaches exact `maxScale`,
    // while keeping `tier 0` & `tier 1` strictly inside `< 1.0f` so they never cross into the next crown band.
    return FractionToTierU32(frac, tier == 2U);
}

// ============================================================================
//  Synchronous Slot Hooks (`0x166CFE1`, `0x1D2A472`, `0x1D2A3CC`, `0x1669CB6`)
// ============================================================================

void PushHistoryFromPostLocked(const ThreadLocalEndemicCtx& t, std::uint32_t tier) {
    if (!t.valid) return;
    for (std::size_t i = kHistoryCap - 1U; i > 0U; --i) {
        g_history[i] = g_history[i - 1U];
    }
    if (g_historyCount < kHistoryCap) ++g_historyCount;

    EndemicHistoryEntry& h = g_history[0];
    h.validSpecies  = true;
    h.validRow      = t.sizeRow.valid;
    h.isCapturePath = t.isCapturePath;
    h.hasRolledSize = false;
    h.animalId      = t.animalId;
    h.animalType    = t.animalType;
    h.rolledTier    = tier;
    h.baseSizeCm    = t.sizeRow.baseSizeCm;
    h.minScale      = t.sizeRow.minScale;
    h.maxScale      = t.sizeRow.maxScale;
    h.rolledScale   = 1.0f;
}

std::uint32_t SimulateCrownTierRaw(const float effProb[3], float totalBase, std::uint32_t u32Val) noexcept {
    if (!(totalBase > kProbEps)) return 1U;
    const float roll = static_cast<float>(static_cast<std::uint64_t>(u32Val)) * (1.0f / 4294967296.0f) * totalBase;
    float cum = 0.0f;
    for (std::uint32_t t = 0U; t < 3U; ++t) {
        if (effProb[t] <= 0.0f) continue;
        cum += effProb[t];
        if (roll <= cum) return t;
    }
    return 1U;
}

void __fastcall OnEndemicCrownTierDrawPre(const MhwRngDrawContext* ctx,
                                          MhwRngHookDecision* ioDecision,
                                          void*) {
    if (!ioDecision) return;

    LiveCrownTierContext sc{};
    ReadCrownTierContextSEH(ctx, sc);

    // Reset per-thread handoff state for the upcoming intra-tier roll & post-draw callback
    tls_lastRoll               = ThreadLocalEndemicCtx{};
    tls_lastRoll.valid         = sc.validSpecies;
    tls_lastRoll.isCapturePath = sc.isCapturePath;
    tls_lastRoll.animalId      = sc.animalId;
    tls_lastRoll.animalType    = sc.animalType;
    tls_lastRoll.validProbs    = sc.validProbs;
    tls_lastRoll.totalBase     = sc.totalBase;
    std::memcpy(tls_lastRoll.effProb, sc.effProb, sizeof(tls_lastRoll.effProb));
    tls_lastRoll.sizeRow       = sc.sizeRow;

    std::lock_guard<std::mutex> lk(g_stateMx);
    const EndemicPolicy* pol = FindActivePolicyLocked(sc.validSpecies, sc.animalId, sc.animalType);

    std::uint32_t chosenTier = 1U;
    if (pol->mode == PolicyMode::kFixedTier) {
        const std::uint32_t u32 = SolveCrownTierU32(sc, pol->targetTier, chosenTier);
        ioDecision->action      = kMhwRngActionReturnValue;
        ioDecision->condId      = pol->condId;
        ioDecision->value0      = u32;
        tls_lastRoll.wasLocked  = true;
        tls_lastRoll.chosenTier = chosenTier;
    } else if (pol->mode == PolicyMode::kExactScale || pol->mode == PolicyMode::kExactCm) {
        float targetScale = pol->exactValue;
        if (pol->mode == PolicyMode::kExactCm) {
            targetScale = (sc.sizeRow.valid && sc.sizeRow.baseSizeCm > 0.0f)
                              ? (pol->exactValue / sc.sizeRow.baseSizeCm)
                              : 1.0f;
        }
        std::uint32_t bestTier = 1U;
        float bestDist = 1e9f;
        if (sc.sizeRow.valid) {
            for (std::uint32_t t = 0U; t < 3U; ++t) {
                if (sc.validProbs && sc.effProb[t] <= 0.0f) continue;
                const float lo = sc.sizeRow.tierMin[t];
                const float hi = sc.sizeRow.tierMax[t];
                const float dist = (targetScale < lo) ? (lo - targetScale)
                                   : ((targetScale > hi) ? (targetScale - hi) : 0.0f);
                if (dist < bestDist) {
                    bestDist = dist;
                    bestTier = t;
                }
            }
        } else {
            bestTier = (targetScale < 0.95f) ? 0U : ((targetScale > 1.05f) ? 2U : 1U);
        }

        const std::uint32_t u32 = SolveCrownTierU32(sc, bestTier, chosenTier);
        ioDecision->action      = kMhwRngActionReturnValue;
        ioDecision->condId      = pol->condId;
        ioDecision->value0      = u32;
        tls_lastRoll.wasLocked  = true;
        tls_lastRoll.chosenTier = chosenTier;

        if (sc.sizeRow.valid) {
            const float lo = sc.sizeRow.tierMin[chosenTier];
            const float hi = sc.sizeRow.tierMax[chosenTier];
            const double frac = (hi > lo) ? std::clamp(static_cast<double>(targetScale - lo) / static_cast<double>(hi - lo), 0.0, 1.0) : 0.5;
            tls_lastRoll.hasExactIntra = true;
            tls_lastRoll.exactIntraU32 = FractionToTierU32(frac, chosenTier == 2U);
        }
    } else {
        // Unlocked: let Selective/ObserveOnly execute pure vanilla RNG; Post callback captures telemetry.
        ioDecision->action = kMhwRngActionUseCondition;
        ioDecision->condId = 0U;
    }
}

void __fastcall OnEndemicCrownTierDrawPost(const MhwRngDrawContext* ctx,
                                           MhwRngHookDecision*,
                                           void*) {
    if (!ctx) return;
    if (!tls_lastRoll.valid) {
        LiveCrownTierContext sc{};
        if (ReadCrownTierContextSEH(ctx, sc)) {
            tls_lastRoll.valid         = sc.validSpecies;
            tls_lastRoll.isCapturePath = sc.isCapturePath;
            tls_lastRoll.animalId      = sc.animalId;
            tls_lastRoll.animalType    = sc.animalType;
            tls_lastRoll.validProbs    = sc.validProbs;
            tls_lastRoll.totalBase     = sc.totalBase;
            std::memcpy(tls_lastRoll.effProb, sc.effProb, sizeof(tls_lastRoll.effProb));
            tls_lastRoll.sizeRow       = sc.sizeRow;
        }
    }
    if (!tls_lastRoll.valid) return;

    const bool isObserve = ((ctx->callFlags & kMhwRngDrawFlagObserveOnly) != 0U);
    if (!tls_lastRoll.wasLocked || isObserve) {
        tls_lastRoll.chosenTier = tls_lastRoll.validProbs
                                      ? SimulateCrownTierRaw(tls_lastRoll.effProb, tls_lastRoll.totalBase, ctx->finalValue0)
                                      : 1U;
    }

    std::lock_guard<std::mutex> lk(g_stateMx);
    PushHistoryFromPostLocked(tls_lastRoll, tls_lastRoll.chosenTier);
}

void HandleIntraTierRollCommon(MhwRngHookDecision* ioDecision,
                               bool useTlsSpecies) {
    std::lock_guard<std::mutex> lk(g_stateMx);
    const bool          haveSp = useTlsSpecies && tls_lastRoll.valid;
    const std::uint32_t aid    = haveSp ? tls_lastRoll.animalId : 0U;
    const std::uint32_t atp    = haveSp ? tls_lastRoll.animalType : kAnyAnimalType;
    const EndemicPolicy* pol   = FindActivePolicyLocked(haveSp, aid, atp);

    if (pol->mode == PolicyMode::kNone) {
        ioDecision->action = kMhwRngActionUseCondition;
        ioDecision->condId = 0U;
        return;
    }

    std::uint32_t chosenU32 = 0x80000000U;
    if ((pol->mode == PolicyMode::kExactScale || pol->mode == PolicyMode::kExactCm) &&
        useTlsSpecies && tls_lastRoll.hasExactIntra) {
        chosenU32 = tls_lastRoll.exactIntraU32;
    } else if (pol->mode == PolicyMode::kFixedTier) {
        const std::uint32_t tier = haveSp ? tls_lastRoll.chosenTier : pol->targetTier;
        chosenU32 = ComputeIntraTierU32(*pol, tier);
    } else {
        chosenU32 = 0x80000000U;
    }

    ioDecision->action = kMhwRngActionReturnValue;
    ioDecision->condId = pol->condId;
    ioDecision->value0 = chosenU32;

    if (haveSp && tls_lastRoll.sizeRow.valid && g_historyCount > 0U &&
        g_history[0].animalId == aid && g_history[0].animalType == atp) {
        const std::uint32_t t = std::min<std::uint32_t>(tls_lastRoll.chosenTier, 2U);
        const float lo = tls_lastRoll.sizeRow.tierMin[t];
        const float hi = tls_lastRoll.sizeRow.tierMax[t];
        const float f  = static_cast<float>(static_cast<std::uint64_t>(chosenU32)) * (1.0f / 4294967296.0f);
        g_history[0].rolledScale   = lo + f * (hi - lo);
        g_history[0].hasRolledSize = true;
    }
}

void __fastcall OnFieldStdIntraTierDraw(const MhwRngDrawContext*,
                                        MhwRngHookDecision* ioDecision,
                                        void*) {
    if (!ioDecision) return;
    HandleIntraTierRollCommon(ioDecision, true);
}

void __fastcall OnFieldSpecIntraTierDraw(const MhwRngDrawContext*,
                                         MhwRngHookDecision* ioDecision,
                                         void*) {
    if (!ioDecision) return;
    HandleIntraTierRollCommon(ioDecision, false);
}

void __fastcall OnCaptureIntraTierDraw(const MhwRngDrawContext*,
                                       MhwRngHookDecision* ioDecision,
                                       void*) {
    if (!ioDecision) return;
    HandleIntraTierRollCommon(ioDecision, true);
}

bool __fastcall FormatEndemicSlotStatus(const MhwRngSlotSnapshot* snap,
                                        char* outBuf,
                                        std::size_t bufCap,
                                        void*) {
    if (!snap || !outBuf || bufCap == 0U) return false;
    const bool en = IsEn();
    std::lock_guard<std::mutex> lk(g_stateMx);
    if (snap->condId == 0U) {
        if (g_globalPolicy.mode != PolicyMode::kNone) {
            std::snprintf(outBuf, bufCap, "%s", FormatPolicySummary(g_globalPolicy, en).c_str());
            return true;
        }
        if (!g_speciesPolicies.empty()) {
            std::snprintf(outBuf, bufCap,
                          en ? "Per-species rules (%zu active)" : u8"按环境生物分流(%zu条专属规则)",
                          g_speciesPolicies.size());
            return true;
        }
        return false;
    }
    for (const auto& p : g_speciesPolicies) {
        if (p.condId == snap->condId && p.mode != PolicyMode::kNone) {
            std::snprintf(outBuf, bufCap, "%s: %s",
                          FormatSpeciesCode(p.animalId, p.animalType).c_str(),
                          FormatPolicySummary(p, en).c_str());
            return true;
        }
    }
    return false;
}

// ============================================================================
//  SEH-Safe Context Readers & Hooks for Rare Endemic / Moly / Mutex Sites
// ============================================================================

void FillRareSetDataFieldsSEH(std::uintptr_t rareSetPtr,
                              RareTlsRollCtx& out,
                              bool copyProbIfMissing) noexcept {
    if (!IsValidUserPtr(rareSetPtr, 0x3ULL)) return;
    out.extra     = *reinterpret_cast<const std::int32_t*>(rareSetPtr + 0x08U); // mTargetWeather
    out.startHour = *reinterpret_cast<const std::uint8_t*>(rareSetPtr + 0x10U);
    out.endHour   = *reinterpret_cast<const std::uint8_t*>(rareSetPtr + 0x11U);
    if (copyProbIfMissing || out.effProb < 0.0f || !std::isfinite(out.effProb)) {
        const float bp = *reinterpret_cast<const float*>(rareSetPtr + 0x0CU);
        if (std::isfinite(bp) && bp >= 0.0f && bp <= 10.0f) out.effProb = bp;
    }
}

// 1. `FUN_141a4a9b0` (`RVA 0x1A4ACED`):
//    `R14D` = animalId, `EBX` = animalType, `R15` = RareSetData*, `XMM6` = effectiveProb,
//    `*(returnAddrSlot + 0xE0)` = isInit (`1` = map load init, `0` = hourly tick).
bool ReadRareMasterCtxSEH(const MhwRngDrawContext* ctx, RareTlsRollCtx& out) noexcept {
    out = RareTlsRollCtx{};
    out.siteKind = 0U;
    if (!ctx || !ctx->callerRegs) return false;
    __try {
        const auto aid = static_cast<std::uint32_t>(ctx->callerRegs->r14);
        const auto atp = static_cast<std::uint32_t>(ctx->callerRegs->rbx);
        out.valid      = (aid > 0U && aid < 1000U && atp < 256U);
        out.animalId   = aid;
        out.animalType = atp;
        if (ctx->returnAddrSlot) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(ctx->returnAddrSlot);
            out.isInit = (bytes[0xE0U] != 0U);
        }
        if ((ctx->callFlags & kMhwRngDrawFlagHasXmmRegs) != 0U) {
            const float xp = ctx->callerRegs->xmm6.f32[0];
            if (std::isfinite(xp) && xp >= 0.0f && xp <= 10.0f) out.effProb = xp;
        }
        FillRareSetDataFieldsSEH(static_cast<std::uintptr_t>(ctx->callerRegs->r15), out, out.effProb < 0.0f);
        return out.valid;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 2. `0x140271ce0` (`RVA 0x0271F25`):
//    `R15` = cAnimalGroupCtrlEc059* (`+0x328` id, `+0x32C` type, `+0x330` regionIdx),
//    `R12` = Ec059RareSetData*, `XMM6` = mProbability.
bool ReadRareMolyCtxSEH(const MhwRngDrawContext* ctx, RareTlsRollCtx& out) noexcept {
    out = RareTlsRollCtx{};
    out.siteKind = 1U;
    out.animalId = 59U;
    if (!ctx || !ctx->callerRegs) return false;
    __try {
        const auto ctrlPtr = static_cast<std::uintptr_t>(ctx->callerRegs->r15);
        if (IsValidUserPtr(ctrlPtr, 0x3ULL)) {
            const auto aid = *reinterpret_cast<const std::uint32_t*>(ctrlPtr + kOffGroupCtrlAnimalId);
            const auto atp = *reinterpret_cast<const std::uint32_t*>(ctrlPtr + kOffGroupCtrlAnimalType);
            const auto reg = *reinterpret_cast<const std::int32_t*>(ctrlPtr + kOffGroupCtrlRegionId);
            if (aid > 0U && aid < 1000U && atp < 256U) {
                out.animalId   = aid;
                out.animalType = atp;
                out.valid      = true;
            } else if (atp < 256U) {
                out.animalId   = 59U;
                out.animalType = atp;
                out.valid      = true;
            }
            out.extra = reg;
        }
        if ((ctx->callFlags & kMhwRngDrawFlagHasXmmRegs) != 0U) {
            const float xp = ctx->callerRegs->xmm6.f32[0];
            if (std::isfinite(xp) && xp >= 0.0f && xp <= 10.0f) out.effProb = xp;
        }
        const auto rareSetPtr = static_cast<std::uintptr_t>(ctx->callerRegs->r12);
        if (IsValidUserPtr(rareSetPtr, 0x3ULL)) {
            out.startHour = *reinterpret_cast<const std::uint8_t*>(rareSetPtr + 0x10U);
            out.endHour   = *reinterpret_cast<const std::uint8_t*>(rareSetPtr + 0x11U);
            if (out.effProb < 0.0f) {
                const float bp = *reinterpret_cast<const float*>(rareSetPtr + 0x0CU);
                if (std::isfinite(bp) && bp >= 0.0f && bp <= 10.0f) out.effProb = bp;
            }
        }
        return out.valid;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 3. `FUN_14166d070` (`RVA 0x166D12E`, caller `0x140271f90` @ `0x0271FFF`):
//    `slotPtr[12]` = return RVA `0x0271FFF`, `slotPtr[13]` = saved `RBX` (`cAnimalGroupCtrl*`),
//    `RDI` = RareSetData*, `XMM6` = mProbability.
bool ReadRareGroupCtxSEH(const MhwRngDrawContext* ctx, RareTlsRollCtx& out) noexcept {
    out = RareTlsRollCtx{};
    out.siteKind = 2U;
    if (!ctx || !ctx->returnAddrSlot || !ctx->exeBase) return false;
    __try {
        const auto* slotPtr = static_cast<const std::uintptr_t*>(ctx->returnAddrSlot);
        if (slotPtr[12] != (ctx->exeBase + kRvaCallerGroupRareRet)) return false;

        const std::uintptr_t ctrlPtr = slotPtr[13];
        if (IsValidUserPtr(ctrlPtr, 0x3ULL)) {
            const auto aid = *reinterpret_cast<const std::uint32_t*>(ctrlPtr + kOffGroupCtrlAnimalId);
            const auto atp = *reinterpret_cast<const std::uint32_t*>(ctrlPtr + kOffGroupCtrlAnimalType);
            if (aid > 0U && aid < 1000U && atp < 256U) {
                out.animalId   = aid;
                out.animalType = atp;
                out.valid      = true;
            }
        }
        if (ctx->callerRegs) {
            if ((ctx->callFlags & kMhwRngDrawFlagHasXmmRegs) != 0U) {
                const float xp = ctx->callerRegs->xmm6.f32[0];
                if (std::isfinite(xp) && xp >= 0.0f && xp <= 10.0f) out.effProb = xp;
            }
            FillRareSetDataFieldsSEH(static_cast<std::uintptr_t>(ctx->callerRegs->rdi), out, out.effProb < 0.0f);
        }
        return out.valid;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 4. `FUN_141a44f50` (`RVA 0x1A44FCC`):
//    `RDI` = `{ uint32_t* slotIdxList, uintptr_t sAnimal, ... }`, `ESI` = `groupCount`.
bool ScanRareMutexGroupSEH(const MhwRngDrawContext* ctx,
                           RareMutexCand* outCands,
                           std::size_t maxCands,
                           std::size_t& outCandCount,
                           std::uint32_t& outGroupCount) noexcept {
    outCandCount  = 0U;
    outGroupCount = 0U;
    if (!ctx || !ctx->callerRegs || !outCands || maxCands == 0U) return false;
    __try {
        const auto paramPtr = static_cast<std::uintptr_t>(ctx->callerRegs->rdi);
        const auto groupCnt = static_cast<std::uint32_t>(ctx->callerRegs->rsi);
        if (!IsValidUserPtr(paramPtr) || groupCnt == 0U || groupCnt > kMaxRareMutexSlots) {
            return false;
        }
        const std::uintptr_t listPtr = *reinterpret_cast<const std::uintptr_t*>(paramPtr + 0x00U);
        const std::uintptr_t sAnimal = *reinterpret_cast<const std::uintptr_t*>(paramPtr + 0x08U);
        if (!IsValidUserPtr(listPtr, 0x3ULL) || !IsValidUserPtr(sAnimal)) {
            return false;
        }

        outGroupCount = groupCnt;
        const auto* slotList = reinterpret_cast<const std::uint32_t*>(listPtr);
        for (std::uint32_t k = 0U; k < groupCnt; ++k) {
            const std::uint32_t slotIdx = slotList[k];
            if (slotIdx >= kMaxRareMutexSlots) continue;
            const std::uintptr_t slotBase =
                sAnimal + kOffSAnimalRareSlotBase + static_cast<std::uintptr_t>(slotIdx) * kRareSlotStride;
            const std::uint8_t justRolled = *reinterpret_cast<const std::uint8_t*>(slotBase + kOffRareSlotJustRolled);
            if (justRolled == 0U) continue;

            RareMutexCand cand{};
            cand.k = k;
            for (std::uint32_t j = 0U; j < 8U; ++j) {
                const std::uintptr_t uAnimal =
                    *reinterpret_cast<const std::uintptr_t*>(slotBase + static_cast<std::uintptr_t>(j) * 0x18U);
                if (!IsValidUserPtr(uAnimal)) continue;
                const auto aid = *reinterpret_cast<const std::uint32_t*>(uAnimal + kOffUAnimalId);
                const auto atp = *reinterpret_cast<const std::uint32_t*>(uAnimal + kOffUAnimalType);
                if (aid > 0U && aid < 1000U && atp < 256U) {
                    cand.validSp    = true;
                    cand.animalId   = aid;
                    cand.animalType = atp;
                    break;
                }
            }
            if (outCandCount < maxCands) {
                outCands[outCandCount++] = cand;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void PushRareHistoryLocked(const RareHistoryEntry& e) {
    for (std::size_t i = kRareHistoryCap - 1U; i > 0U; --i) {
        g_rareHistory[i] = g_rareHistory[i - 1U];
    }
    if (g_rareHistoryCount < kRareHistoryCap) ++g_rareHistoryCount;
    g_rareHistory[0] = e;
}

void FormatRareEntryText(const RareHistoryEntry& e, bool en, char* buf, std::size_t cap) {
    const std::string sp = e.validSp ? FormatSpeciesCode(e.animalId, e.animalType) : std::string("-");
    const char* outcome  = e.spawned ? (en ? "SPAWN" : u8"命中") : (en ? "MISS" : u8"未出");
    const char* ovTag    = e.overridden ? (en ? "[OVR]" : u8"[改]") : "";
    if (e.siteKind == 1U) {
        std::snprintf(buf, cap,
                      en ? "[endemic/rare] %s%s %s -> %s | RegionIdx:%d | Prob:%.2f%%"
                         : u8"[endemic/rare] %s%s %s -> %s | 聚魔地区编号:%d | 概率:%.2f%%",
                      RareSiteTag(1U, en), ovTag, sp.c_str(), outcome,
                      e.extra,
                      e.effProb >= 0.0f ? static_cast<double>(e.effProb) * 100.0 : 0.0);
    } else if (e.siteKind == 3U) {
        std::snprintf(buf, cap,
                      en ? "[endemic/rare] %s%s %s -> Slot#%d"
                         : u8"[endemic/rare] %s%s %s -> 胜出槽位#%d",
                      RareSiteTag(3U, en), ovTag, sp.c_str(), e.extra);
    } else {
        const char* phaseTag = e.isInit ? (en ? "Init" : u8"进图") : (en ? "Hour" : u8"整点");
        std::snprintf(buf, cap,
                      en ? "[endemic/rare] %s%s(%s) %s -> %s | Prob:%.2f%% | %s %u-%uh"
                         : u8"[endemic/rare] %s%s(%s) %s -> %s | 概率:%.2f%% | %s %u-%uh",
                      RareSiteTag(e.siteKind, en), ovTag, phaseTag, sp.c_str(), outcome,
                      e.effProb >= 0.0f ? static_cast<double>(e.effProb) * 100.0 : 0.0,
                      WeatherGateTag(e.extra, en),
                      static_cast<unsigned>(e.startHour), static_cast<unsigned>(e.endHour));
    }
}

void EmitRareLogAsync(const RareHistoryEntry& e) {
    if (!g_host || !g_host->ChatPrintfAsync) return;
    char line[256]{};
    FormatRareEntryText(e, IsEn(), line, sizeof(line));
    g_host->ChatPrintfAsync("%s", line);
}

void HandleSimpleRarePre(RareTlsRollCtx& tls, MhwRngHookDecision* ioDecision) {
    std::lock_guard<std::mutex> lk(g_stateMx);
    const RarePolicy* pol = FindActiveRarePolicyLocked(tls.valid, tls.animalId, tls.animalType);
    if (pol->mode == RareMode::kForceOn) {
        ioDecision->action = kMhwRngActionReturnValue;
        ioDecision->condId = pol->condId;
        ioDecision->value0 = kRareForceU32;
        tls.overridden     = true;
    } else if (pol->mode == RareMode::kForceOff) {
        ioDecision->action = kMhwRngActionReturnValue;
        ioDecision->condId = pol->condId;
        ioDecision->value0 = kRareBlockU32;
        tls.overridden     = true;
    } else {
        ioDecision->action = kMhwRngActionUseCondition;
        ioDecision->condId = 0U;
    }
}

template <bool (*ReadFn)(const MhwRngDrawContext*, RareTlsRollCtx&)>
void HandleSimpleRarePost(const MhwRngDrawContext* ctx, std::uint8_t expectedKind) {
    if (!ctx) return;
    const bool isObserve = ((ctx->callFlags & kMhwRngDrawFlagObserveOnly) != 0U);
    if (!tls_lastRareRoll.valid || tls_lastRareRoll.siteKind != expectedKind) {
        if (!ReadFn(ctx, tls_lastRareRoll)) return;
    }

    const float roll = static_cast<float>(static_cast<std::uint64_t>(ctx->finalValue0)) * (1.0f / 4294967296.0f);
    const bool spawned = (tls_lastRareRoll.effProb >= 0.0f)
                             ? (roll <= tls_lastRareRoll.effProb)
                             : (ctx->finalValue0 == kRareForceU32);

    RareHistoryEntry entry{};
    entry.siteKind   = expectedKind;
    entry.validSp    = tls_lastRareRoll.valid;
    entry.spawned    = spawned;
    entry.overridden = tls_lastRareRoll.overridden && !isObserve;
    entry.isInit     = tls_lastRareRoll.isInit;
    entry.animalId   = tls_lastRareRoll.animalId;
    entry.animalType = tls_lastRareRoll.animalType;
    entry.extra      = tls_lastRareRoll.extra;
    entry.effProb    = tls_lastRareRoll.effProb;
    entry.startHour  = tls_lastRareRoll.startHour;
    entry.endHour    = tls_lastRareRoll.endHour;

    bool emitLog = false;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        PushRareHistoryLocked(entry);
        emitLog = g_rareLogEnabled && (spawned || entry.overridden);
    }
    tls_lastRareRoll = RareTlsRollCtx{};
    if (emitLog) {
        EmitRareLogAsync(entry);
        if (spawned && entry.overridden && entry.validSp) {
            const bool en = IsEn();
            const std::string sp = FormatSpeciesCode(entry.animalId, entry.animalType);
            char popupBuf[192]{};
            std::snprintf(popupBuf, sizeof(popupBuf),
                          en ? "[endemic/rare] Rare spawn active: %s"
                             : u8"[环境生物/稀有] 已触发稀有出现: %s",
                          sp.c_str());
            MhwWorld_TryDisplayPopup(g_host, popupBuf, 3.0f, 0.0f, 0U);
        }
    }
}

void __fastcall OnRareMasterDrawPre(const MhwRngDrawContext* ctx, MhwRngHookDecision* ioDecision, void*) {
    if (!ioDecision) return;
    ReadRareMasterCtxSEH(ctx, tls_lastRareRoll);
    HandleSimpleRarePre(tls_lastRareRoll, ioDecision);
}

void __fastcall OnRareMasterDrawPost(const MhwRngDrawContext* ctx, MhwRngHookDecision*, void*) {
    HandleSimpleRarePost<&ReadRareMasterCtxSEH>(ctx, 0U);
}

void __fastcall OnRareMolyDrawPre(const MhwRngDrawContext* ctx, MhwRngHookDecision* ioDecision, void*) {
    if (!ioDecision) return;
    ReadRareMolyCtxSEH(ctx, tls_lastRareRoll);
    HandleSimpleRarePre(tls_lastRareRoll, ioDecision);
}

void __fastcall OnRareMolyDrawPost(const MhwRngDrawContext* ctx, MhwRngHookDecision*, void*) {
    HandleSimpleRarePost<&ReadRareMolyCtxSEH>(ctx, 1U);
}

void __fastcall OnRareGroupDrawPre(const MhwRngDrawContext* ctx, MhwRngHookDecision* ioDecision, void*) {
    if (!ioDecision) return;
    if (!ReadRareGroupCtxSEH(ctx, tls_lastRareRoll)) {
        ioDecision->action = kMhwRngActionUseCondition;
        ioDecision->condId = 0U;
        return;
    }
    HandleSimpleRarePre(tls_lastRareRoll, ioDecision);
}

void __fastcall OnRareGroupDrawPost(const MhwRngDrawContext* ctx, MhwRngHookDecision*, void*) {
    HandleSimpleRarePost<&ReadRareGroupCtxSEH>(ctx, 2U);
}

void __fastcall OnRareMutexDrawPre(const MhwRngDrawContext* ctx, MhwRngHookDecision* ioDecision, void*) {
    if (!ioDecision) return;
    tls_lastRareMutex = RareMutexTlsCtx{};

    RareMutexCand cands[kMaxRareMutexSlots]{};
    std::size_t   candCount  = 0U;
    std::uint32_t groupCount = 0U;
    if (!ScanRareMutexGroupSEH(ctx, cands, kMaxRareMutexSlots, candCount, groupCount)) {
        ioDecision->action = kMhwRngActionUseCondition;
        ioDecision->condId = 0U;
        return;
    }

    tls_lastRareMutex.valid      = true;
    tls_lastRareMutex.groupCount = groupCount;
    tls_lastRareMutex.candCount  = candCount;
    if (candCount > 0U) {
        std::memcpy(tls_lastRareMutex.cands, cands, candCount * sizeof(RareMutexCand));
    }

    std::uint32_t speciesIndices[kMaxRareMutexSlots]{};
    std::size_t   speciesCount = 0U;
    std::uint32_t anyForceIndices[kMaxRareMutexSlots]{};
    std::size_t   anyForceCount = 0U;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        if (!HasAnyRareForceOnLocked() || candCount == 0U) {
            ioDecision->action = kMhwRngActionUseCondition;
            ioDecision->condId = 0U;
            return;
        }
        for (std::size_t i = 0U; i < candCount; ++i) {
            const RareMutexCand& c = cands[i];
            bool isSpRule = false;
            const RarePolicy* pol = FindActiveRarePolicyLocked(c.validSp, c.animalId, c.animalType, &isSpRule);
            if (pol->mode == RareMode::kForceOn) {
                anyForceIndices[anyForceCount++] = c.k;
                if (isSpRule) {
                    speciesIndices[speciesCount++] = c.k;
                }
            }
        }
    }

    const std::uint32_t* pool = (speciesCount > 0U) ? speciesIndices : anyForceIndices;
    const std::size_t    cnt  = (speciesCount > 0U) ? speciesCount   : anyForceCount;
    if (cnt == 0U) {
        ioDecision->action = kMhwRngActionUseCondition;
        ioDecision->condId = 0U;
        return;
    }

    std::size_t pick = 0U;
    if (cnt > 1U && ctx && ctx->DrawDeterministic) {
        pick = static_cast<std::size_t>(ctx->DrawDeterministic(kRvaRareMutexTieBreak, 0U)) % cnt;
    }
    const std::uint32_t winnerK  = pool[pick];
    tls_lastRareMutex.overridden = true;
    tls_lastRareMutex.winnerK    = winnerK;
    ioDecision->action = kMhwRngActionReturnValue;
    ioDecision->condId = 0U;
    ioDecision->value0 = winnerK;
}

void __fastcall OnRareMutexDrawPost(const MhwRngDrawContext* ctx, MhwRngHookDecision*, void*) {
    if (!ctx) return;
    const bool isObserve = ((ctx->callFlags & kMhwRngDrawFlagObserveOnly) != 0U);
    if (!tls_lastRareMutex.valid) {
        if (!ScanRareMutexGroupSEH(ctx, tls_lastRareMutex.cands, kMaxRareMutexSlots,
                                   tls_lastRareMutex.candCount, tls_lastRareMutex.groupCount)) {
            return;
        }
        tls_lastRareMutex.valid = true;
    }
    const std::uint32_t gCnt = tls_lastRareMutex.groupCount;
    if (gCnt == 0U) {
        tls_lastRareMutex = RareMutexTlsCtx{};
        return;
    }

    const bool wasOverridden = tls_lastRareMutex.overridden && !isObserve;
    const std::uint32_t winK = wasOverridden ? tls_lastRareMutex.winnerK : (ctx->finalValue0 % gCnt);

    RareHistoryEntry entry{};
    entry.siteKind   = 3U;
    entry.overridden = wasOverridden;
    entry.extra      = static_cast<std::int32_t>(winK);
    for (std::size_t i = 0U; i < tls_lastRareMutex.candCount; ++i) {
        if (tls_lastRareMutex.cands[i].k == winK) {
            entry.spawned    = true;
            entry.validSp    = tls_lastRareMutex.cands[i].validSp;
            entry.animalId   = tls_lastRareMutex.cands[i].animalId;
            entry.animalType = tls_lastRareMutex.cands[i].animalType;
            break;
        }
    }

    bool emitLog = false;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        PushRareHistoryLocked(entry);
        emitLog = g_rareLogEnabled && (wasOverridden || entry.spawned);
    }
    tls_lastRareMutex = RareMutexTlsCtx{};
    if (emitLog) {
        EmitRareLogAsync(entry);
        if (entry.spawned && entry.overridden && entry.validSp) {
            const bool en = IsEn();
            const std::string sp = FormatSpeciesCode(entry.animalId, entry.animalType);
            char popupBuf[192]{};
            std::snprintf(popupBuf, sizeof(popupBuf),
                          en ? "[endemic/rare] Rare spawn active: %s"
                             : u8"[环境生物/稀有] 已触发稀有出现: %s",
                          sp.c_str());
            MhwWorld_TryDisplayPopup(g_host, popupBuf, 3.0f, 0.0f, 0U);
        }
    }
}

bool __fastcall FormatEndemicRareSlotStatus(const MhwRngSlotSnapshot* snap,
                                            char* outBuf,
                                            std::size_t bufCap,
                                            void*) {
    if (!snap || !outBuf || bufCap == 0U) return false;
    const bool en = IsEn();
    std::lock_guard<std::mutex> lk(g_stateMx);
    if (snap->condId == 0U) {
        if (g_globalRarePolicy.mode != RareMode::kVanilla) {
            std::snprintf(outBuf, bufCap, "%s", FormatRareModeSummary(g_globalRarePolicy.mode, en));
            return true;
        }
        if (!g_speciesRarePolicies.empty()) {
            std::snprintf(outBuf, bufCap,
                          en ? "Per-species rare rules (%zu active)" : u8"按物种稀有分流(%zu条专属规则)",
                          g_speciesRarePolicies.size());
            return true;
        }
        return false;
    }
    for (const auto& p : g_speciesRarePolicies) {
        if (p.condId == snap->condId && p.mode != RareMode::kVanilla) {
            std::snprintf(outBuf, bufCap, "%s: %s",
                          FormatSpeciesCode(p.animalId, p.animalType).c_str(),
                          FormatRareModeSummary(p.mode, en));
            return true;
        }
    }
    return false;
}

// ============================================================================
//  `$rng endemic ...` Subcommand Parsing & Telemetry Output
// ============================================================================

enum class ParseResult : std::uint8_t {
    kOk = 0,
    kBadSyntax,
    kReversedOrderHint,
    kOutOfRange
};

bool ParseDoubleStrict(const char* s, double& outVal, bool allowTrailingPct = false) noexcept {
    if (!s || s[0] == '\0') return false;
    char* ep = nullptr;
    const double v = std::strtod(s, &ep);
    if (ep == s || !std::isfinite(v)) return false;
    if (*ep == '\0') { outVal = v; return true; }
    if (allowTrailingPct && *ep == '%' && ep[1] == '\0') { outVal = v; return true; }
    return false;
}

ParseResult ParsePolicyTokens(int argc,
                              const char* const* argv,
                              std::uint32_t animalId,
                              std::uint32_t animalType,
                              EndemicPolicy& outPol) {
    if (argc < 1) return ParseResult::kBadSyntax;
    const std::string t0 = LowerAscii(argv[0]);
    const std::uint32_t condId = MakeSpeciesCondId(animalId, animalType);

    if (argc >= 2 && t0 != "exact") {
        std::uint32_t tmpId = 0, tmpTp = 0;
        if (ParseSpeciesSelector(argv[argc - 1], tmpId, tmpTp)) {
            return ParseResult::kReversedOrderHint;
        }
    }

    auto parseOptionalIntra = [&](double defaultPct, double& outIntra) -> ParseResult {
        if (argc == 1) { outIntra = defaultPct; return ParseResult::kOk; }
        if (argc > 2) return ParseResult::kBadSyntax;
        double p = 0.0;
        if (!ParseDoubleStrict(argv[1], p, true)) return ParseResult::kBadSyntax;
        if (p < 0.0 || p > 100.0) return ParseResult::kOutOfRange;
        outIntra = p;
        return ParseResult::kOk;
    };

    if (t0 == "min" || t0 == "mini" || t0 == "small") {
        double intra = 0.0;
        const ParseResult pr = parseOptionalIntra(0.0, intra);
        if (pr != ParseResult::kOk) return pr;
        outPol = EndemicPolicy{PolicyMode::kFixedTier, animalId, animalType, condId, 0U, intra, 1.0f};
        return ParseResult::kOk;
    }

    if (t0 == "normal" || t0 == "mid" || t0 == "std") {
        double intra = 50.0;
        const ParseResult pr = parseOptionalIntra(50.0, intra);
        if (pr != ParseResult::kOk) return pr;
        outPol = EndemicPolicy{PolicyMode::kFixedTier, animalId, animalType, condId, 1U, intra, 1.0f};
        return ParseResult::kOk;
    }

    if (t0 == "max" || t0 == "giant" || t0 == "large" || t0 == "big") {
        double intra = 100.0;
        const ParseResult pr = parseOptionalIntra(100.0, intra);
        if (pr != ParseResult::kOk) return pr;
        outPol = EndemicPolicy{PolicyMode::kFixedTier, animalId, animalType, condId, 2U, intra, 1.0f};
        return ParseResult::kOk;
    }

    std::string exStr;
    if (t0 == "exact") {
        if (argc != 2) return ParseResult::kBadSyntax;
        exStr = LowerAscii(argv[1]);
    } else if (t0.rfind("exact:", 0) == 0 && t0.size() > 6U) {
        if (argc != 1) return ParseResult::kBadSyntax;
        exStr = LowerAscii(std::string(argv[0]).substr(6U));
    }
    if (!exStr.empty()) {
        if (exStr.size() > 2U && exStr.substr(exStr.size() - 2U) == "cm") {
            double cm = 0.0;
            if (!ParseDoubleStrict(exStr.substr(0U, exStr.size() - 2U).c_str(), cm)) return ParseResult::kBadSyntax;
            if (cm <= 0.0 || cm > 10000.0) return ParseResult::kOutOfRange;
            outPol = EndemicPolicy{PolicyMode::kExactCm, animalId, animalType, condId, 1U, 50.0, static_cast<float>(cm)};
            return ParseResult::kOk;
        }
        if (exStr.back() == '%') {
            double pct = 0.0;
            if (!ParseDoubleStrict(exStr.substr(0U, exStr.size() - 1U).c_str(), pct)) return ParseResult::kBadSyntax;
            if (pct < 30.0 || pct > 300.0) return ParseResult::kOutOfRange;
            outPol = EndemicPolicy{PolicyMode::kExactScale, animalId, animalType, condId, 1U, 50.0, static_cast<float>(pct / 100.0)};
            return ParseResult::kOk;
        }
        if (exStr.back() == 'x') exStr.pop_back();
        double val = 0.0;
        if (!ParseDoubleStrict(exStr.c_str(), val)) return ParseResult::kBadSyntax;
        if (val >= 0.3 && val <= 3.0) {
            outPol = EndemicPolicy{PolicyMode::kExactScale, animalId, animalType, condId, 1U, 50.0, static_cast<float>(val)};
            return ParseResult::kOk;
        }
        if (val >= 30.0 && val <= 300.0) {
            outPol = EndemicPolicy{PolicyMode::kExactScale, animalId, animalType, condId, 1U, 50.0, static_cast<float>(val / 100.0)};
            return ParseResult::kOk;
        }
        return ParseResult::kOutOfRange;
    }

    return ParseResult::kBadSyntax;
}

void PrintEndemicHistory(bool showAll) {
    const bool en = IsEn();
    EndemicHistoryEntry snap[kHistoryCap]{};
    std::size_t count = 0U;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        count = g_historyCount;
        for (std::size_t i = 0; i < count; ++i) snap[i] = g_history[i];
    }
    if (count == 0U) {
        if (en) {
            g_host->ChatPrint(showAll
                                  ? "[endemic] No endemic size rolls recorded yet; enter a quest/expedition or capture one"
                                  : "[endemic] Usage: `$rng endemic min|normal|max|exact 1.2x|exact 30cm` or `$rng endemic ec059 max`");
        } else {
            g_host->ChatPrint(showAll
                                  ? u8"[endemic] 尚无环境生物尺寸抽取记录；进入探索/任务刷出或捕获环境生物后自动记录"
                                  : u8"[endemic] 用法: `$rng endemic min|normal|max|exact 1.2x|exact 30cm` 或 `$rng endemic ec059 max`");
        }
        return;
    }

    const std::size_t limit = showAll ? count : 1U;
    for (std::size_t i = 0; i < limit; ++i) {
        const auto& e = snap[i];
        const std::string code = FormatSpeciesCode(e.animalId, e.animalType);
        const char* srcTag = e.isCapturePath ? (en ? "Cap" : u8"捕获") : (en ? "Spawn" : u8"刷新");
        if (e.validRow && e.baseSizeCm > 0.0f && e.hasRolledSize) {
            g_host->ChatPrintf(en ? "[endemic] %s#%zu %s [%s] | Allowed:%.1f~%.1fcm | Rolled:%.2fcm(%.0f%%)"
                                  : u8"[endemic] %s#%zu %s [%s] | 允许:%.1f~%.1fcm | 抽出:%.2fcm(%.0f%%)",
                               srcTag, i + 1U, code.c_str(), TierName(e.rolledTier, en),
                               e.baseSizeCm * e.minScale, e.baseSizeCm * e.maxScale,
                               e.baseSizeCm * e.rolledScale, e.rolledScale * 100.0f);
        } else if (e.validRow && e.baseSizeCm > 0.0f) {
            g_host->ChatPrintf(en ? "[endemic] %s#%zu %s [%s] | Allowed:%.1f~%.1fcm"
                                  : u8"[endemic] %s#%zu %s [%s] | 允许:%.1f~%.1fcm",
                               srcTag, i + 1U, code.c_str(), TierName(e.rolledTier, en),
                               e.baseSizeCm * e.minScale, e.baseSizeCm * e.maxScale);
        } else {
            g_host->ChatPrintf("[endemic] %s#%zu %s [%s]",
                               srcTag, i + 1U, code.c_str(), TierName(e.rolledTier, en));
        }
    }
}

void PrintRareHistory(bool showAll) {
    const bool en = IsEn();
    RareHistoryEntry snap[kRareHistoryCap]{};
    std::size_t count = 0U;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        count = g_rareHistoryCount;
        for (std::size_t i = 0U; i < count; ++i) snap[i] = g_rareHistory[i];
    }
    if (count == 0U) {
        g_host->ChatPrint(en ? "[endemic/rare] No rare spawn rolls recorded yet; enter a quest/expedition first"
                             : u8"[endemic/rare] 尚无稀有生物抽签记录；进入探索/任务/聚魔后自动记录");
        return;
    }
    const std::size_t limit = showAll ? count : 1U;
    for (std::size_t i = 0U; i < limit; ++i) {
        char line[256]{};
        FormatRareEntryText(snap[i], en, line, sizeof(line));
        g_host->ChatPrintf("#%zu %s", i + 1U, line);
    }
}

void PrintRareStatus() {
    const bool en = IsEn();
    RarePolicy              globalRare{};
    std::vector<RarePolicy> spRare;
    bool                    logEnabled = false;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        globalRare = g_globalRarePolicy;
        spRare     = g_speciesRarePolicies;
        logEnabled = g_rareLogEnabled;
    }
    g_host->ChatPrintf(en ? "[endemic/rare] Global: %s | Species rules: %zu | Log: %s"
                          : u8"[endemic/rare] 全局稀有: %s | 专属规则: %zu 条 | 播报: %s",
                       FormatRareModeSummary(globalRare.mode, en),
                       spRare.size(),
                       logEnabled ? "on" : "off");
    const std::size_t showN = std::min<std::size_t>(spRare.size(), 4U);
    for (std::size_t i = 0U; i < showN; ++i) {
        const auto& p = spRare[i];
        g_host->ChatPrintf(en ? "[endemic/rare] Rule %s -> %s" : u8"[endemic/rare] 专属 %s -> %s",
                           FormatSpeciesCode(p.animalId, p.animalType).c_str(),
                           FormatRareModeSummary(p.mode, en));
    }
    if (spRare.size() > showN) {
        g_host->ChatPrintf(en ? "[endemic/rare] ...and %zu more rare rule(s)" : u8"[endemic/rare] ...另有 %zu 条稀有专属规则",
                           spRare.size() - showN);
    }
    MhwWorldState ws{};
    if (MhwWorld_TryGetState(g_host, &ws) && ws.inScene != 0U) {
        const char* wxStr = ws.hasTime
                                ? (ws.isSpecialWeather ? (en ? "Special" : u8"特殊天气") : (en ? "Clear" : u8"常规"))
                                : "?";
        if (ws.isGuidingLands != 0U) {
            g_host->ChatPrintf(en ? "[endemic/rare] Live World: Stage %u | Hour %.1f | Weather %u (%s) | GL Lv %u/%u/%u/%u/%u/%u"
                                  : u8"[endemic/rare] 当前世界: 地图 %u | 时间 %.1fh | 天气 %u (%s) | 聚魔 %u/%u/%u/%u/%u/%u",
                               ws.stageId, static_cast<double>(ws.gameHour), ws.weatherId, wxStr,
                               static_cast<unsigned>(ws.glRegionLevels[0]), static_cast<unsigned>(ws.glRegionLevels[1]),
                               static_cast<unsigned>(ws.glRegionLevels[2]), static_cast<unsigned>(ws.glRegionLevels[3]),
                               static_cast<unsigned>(ws.glRegionLevels[4]), static_cast<unsigned>(ws.glRegionLevels[5]));
        } else {
            g_host->ChatPrintf(en ? "[endemic/rare] Live World: Stage %u | Hour %.1f | Weather %u (%s)"
                                  : u8"[endemic/rare] 当前世界: 地图 %u | 时间 %.1fh | 天气 %u (%s)",
                               ws.stageId, static_cast<double>(ws.gameHour), ws.weatherId, wxStr);
        }
    }
    PrintRareHistory(false);
}

void ClearRareSlotConditions() {
    if (!g_host) return;
    g_host->ClearSlotAllConditions(kRvaRareMasterRoll);
    g_host->ClearSlotAllConditions(kRvaRareMolyRoll);
    g_host->ClearSlotAllConditions(kRvaRareGroupRoll);
    g_host->ClearSlotAllConditions(kRvaRareMutexTieBreak);
}

bool ParseRareModeToken(const std::string& tok, RareMode& outMode, bool& outIsClear) noexcept {
    outIsClear = false;
    if (tok == "on" || tok == "100" || tok == "100%" || tok == "force" || tok == "1") {
        outMode = RareMode::kForceOn;
        return true;
    }
    if (tok == "0" || tok == "0%" || tok == "block" || tok == "none" || tok == "deny") {
        outMode = RareMode::kForceOff;
        return true;
    }
    if (tok == "off" || tok == "clear" || tok == "reset" || tok == "vanilla") {
        outMode    = RareMode::kVanilla;
        outIsClear = true;
        return true;
    }
    return false;
}

bool RemoveSpeciesRareRuleLocked(std::uint32_t aid, std::uint32_t atp) {
    const std::size_t before = g_speciesRarePolicies.size();
    g_speciesRarePolicies.erase(
        std::remove_if(g_speciesRarePolicies.begin(), g_speciesRarePolicies.end(),
                       [&](const RarePolicy& p) { return p.animalId == aid && p.animalType == atp; }),
        g_speciesRarePolicies.end());
    return g_speciesRarePolicies.size() != before;
}

void ApplySpeciesRareRule(std::uint32_t aid, std::uint32_t atp, RareMode mode, bool isClear) {
    const bool en = IsEn();
    const std::string code = FormatSpeciesCode(aid, atp);
    if (isClear || mode == RareMode::kVanilla) {
        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            RemoveSpeciesRareRuleLocked(aid, atp);
        }
        g_host->ChatPrintf(en ? "[endemic/rare] Removed rare rule for %s"
                              : u8"[endemic/rare] 已移除 %s 的稀有专属规则",
                           code.c_str());
        return;
    }
    const RarePolicy pol{mode, aid, atp, MakeSpeciesCondId(aid, atp)};
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        bool updated = false;
        for (auto& existing : g_speciesRarePolicies) {
            if (existing.animalId == aid && existing.animalType == atp) {
                existing = pol;
                updated  = true;
                break;
            }
        }
        if (!updated) g_speciesRarePolicies.push_back(pol);
    }
    g_host->ChatPrintf(en ? "[endemic/rare] Set %s -> %s (respects vanilla weather/time/level gates)"
                          : u8"[endemic/rare] 已设专属 %s -> %s (仍遵守原版天气/时段/聚魔等级条件)",
                       code.c_str(), FormatRareModeSummary(mode, en));
}

void PrintRareHelp() {
    const bool en = IsEn();
    if (en) {
        g_host->ChatPrint("[endemic/rare] Pure-RNG mode (respects weather, time-of-day & Guiding Lands level):");
        g_host->ChatPrint("[endemic/rare] Global: `$rng endemic rare on|0%|off` | Species: `$rng endemic rare ec058 on|0%|off` or `rare wintermoon_nettle on`");
        g_host->ChatPrint("[endemic/rare] Log/History: `$rng endemic rare log [on|off]` | `history` | `status`");
    } else {
        g_host->ChatPrint(u8"[endemic/rare] 纯 RNG 模式(含大型个体鱼与地区稀有毛吉，仍遵守原版天气/时段/聚魔等级):");
        g_host->ChatPrint(u8"[endemic/rare] 全局: `$rng endemic rare on|0%|off` | 按物种: `$rng endemic rare ec058 on|0%|off` 或 `rare 月羽天母 on` (支持简中名/英文别名)");
        g_host->ChatPrint(u8"[endemic/rare] 播报/记录: `$rng endemic rare log [on|off]` | `history` | `status`");
    }
}

void HandleRareSubcommand(int argc, const char* const* argv) {
    const bool en = IsEn();
    if (argc == 0) {
        PrintRareStatus();
        return;
    }
    const std::string r0 = LowerAscii(argv[0]);
    if (r0 == "status" || r0 == "info") {
        PrintRareStatus();
        return;
    }
    if (r0 == "help" || r0 == "?") {
        PrintRareHelp();
        return;
    }
    if (r0 == "history" || r0 == "recent") {
        PrintRareHistory(true);
        return;
    }
    if (r0 == "log") {
        bool nextLog = false;
        if (argc >= 2) {
            const std::string lv = LowerAscii(argv[1]);
            if (lv == "on" || lv == "1" || lv == "true") {
                std::lock_guard<std::mutex> lk(g_stateMx);
                g_rareLogEnabled = true;
                nextLog          = true;
            } else if (lv == "off" || lv == "0" || lv == "false") {
                std::lock_guard<std::mutex> lk(g_stateMx);
                g_rareLogEnabled = false;
                nextLog          = false;
            } else {
                g_host->ChatPrint(en ? "[endemic/rare] Usage: `$rng endemic rare log [on|off]`"
                                     : u8"[endemic/rare] 用法: `$rng endemic rare log [on|off]`");
                return;
            }
        } else {
            std::lock_guard<std::mutex> lk(g_stateMx);
            g_rareLogEnabled = !g_rareLogEnabled;
            nextLog          = g_rareLogEnabled;
        }
        g_host->ChatPrintf(en ? "[endemic/rare] Live rare spawn log: %s"
                              : u8"[endemic/rare] 稀有生物刷新实时播报: %s",
                           nextLog ? "on" : "off");
        return;
    }

    std::uint32_t aid = 0U, atp = kAnyAnimalType;
    if (ParseSpeciesSelector(argv[0], aid, atp)) {
        if (argc < 2) {
            g_host->ChatPrintf(en ? "[endemic/rare] Usage: `$rng endemic rare %s <on|0%|off>`"
                                  : u8"[endemic/rare] 用法: `$rng endemic rare %s <on|0%|off>`",
                               argv[0]);
            return;
        }
        RareMode mode = RareMode::kVanilla;
        bool isClear = false;
        if (!ParseRareModeToken(LowerAscii(argv[1]), mode, isClear) || argc > 2) {
            g_host->ChatPrint(en ? "[endemic/rare] Invalid mode; use `on` / `0%` / `block` / `off`"
                                 : u8"[endemic/rare] 参数无效；支持 `on` / `0%` / `block` / `off`");
            return;
        }
        ApplySpeciesRareRule(aid, atp, mode, isClear);
        return;
    }

    RareMode mode = RareMode::kVanilla;
    bool isClear = false;
    if (!ParseRareModeToken(r0, mode, isClear)) {
        g_host->ChatPrintf(en ? "[endemic/rare] Unknown arg `%s`; type `$rng endemic rare help`"
                              : u8"[endemic/rare] 未知参数 `%s`；输 `$rng endemic rare help` 查看用法",
                           argv[0]);
        return;
    }

    if (isClear) {
        if (argc >= 2 && ParseSpeciesSelector(argv[1], aid, atp)) {
            ApplySpeciesRareRule(aid, atp, RareMode::kVanilla, true);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            g_globalRarePolicy = RarePolicy{};
            g_speciesRarePolicies.clear();
        }
        ClearRareSlotConditions();
        g_host->ChatPrint(en ? "[endemic/rare] Cleared rare spawn rules -> vanilla RNG"
                             : u8"[endemic/rare] 已清空稀有生物生成规则 -> 恢复正常随机");
        return;
    }

    if (argc >= 2 && ParseSpeciesSelector(argv[1], aid, atp)) {
        g_host->ChatPrintf(en ? "[endemic/rare] Order hint: put species first, e.g. `$rng endemic rare %s %s`"
                              : u8"[endemic/rare] 顺序提示: 按物种设置请把代号放前面，例如 `$rng endemic rare %s %s`",
                           argv[1], argv[0]);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        g_globalRarePolicy = RarePolicy{mode, 0U, kAnyAnimalType, 0U};
    }
    g_host->ChatPrintf(en ? "[endemic/rare] Global rare spawn -> %s"
                          : u8"[endemic/rare] 已设全局稀有生成 -> %s",
                       FormatRareModeSummary(mode, en));
}

void PrintEndemicStatus() {
    const bool en = IsEn();
    EndemicPolicy              globalSnap{};
    std::vector<EndemicPolicy> spSnaps;
    RareMode                   rareMode = RareMode::kVanilla;
    std::size_t                rareSpCnt = 0U;
    bool                       rareLog   = false;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        globalSnap = g_globalPolicy;
        spSnaps    = g_speciesPolicies;
        rareMode   = g_globalRarePolicy.mode;
        rareSpCnt  = g_speciesRarePolicies.size();
        rareLog    = g_rareLogEnabled;
    }

    g_host->ChatPrintf(en ? "[endemic] Global size: %s | Size rules: %zu"
                          : u8"[endemic] 全局尺寸: %s | 尺寸专属规则: %zu 条",
                       FormatPolicySummary(globalSnap, en).c_str(),
                       spSnaps.size());
    const std::size_t showN = std::min<std::size_t>(spSnaps.size(), 4U);
    for (std::size_t i = 0; i < showN; ++i) {
        const auto& p = spSnaps[i];
        g_host->ChatPrintf(en ? "[endemic] Rule %s -> %s" : u8"[endemic] 专属 %s -> %s",
                           FormatSpeciesCode(p.animalId, p.animalType).c_str(),
                           FormatPolicySummary(p, en).c_str());
    }
    if (spSnaps.size() > showN) {
        g_host->ChatPrintf(en ? "[endemic] ...and %zu more rule(s) active" : u8"[endemic] ...另有 %zu 条专属规则生效中",
                           spSnaps.size() - showN);
    }
    g_host->ChatPrintf(en ? "[endemic] Rare spawn: %s | Rare rules: %zu | Log: %s (`$rng endemic rare`)"
                          : u8"[endemic] 稀有生成: %s | 稀有专属: %zu 条 | 播报: %s (`$rng endemic rare`)",
                       FormatRareModeSummary(rareMode, en),
                       rareSpCnt,
                       rareLog ? "on" : "off");
    PrintEndemicHistory(false);
}

void __fastcall Cmd_Endemic(int argc, const char* const* argv, const char*, void*) {
    if (!g_host) return;
    const bool en = IsEn();
    if (argc == 0) {
        PrintEndemicStatus();
        return;
    }

    const std::string a0 = LowerAscii(argv[0]);
    if (a0 == "rare") {
        HandleRareSubcommand(argc - 1, argv + 1);
        return;
    }
    if (a0 == "status" || a0 == "info") {
        PrintEndemicStatus();
        return;
    }
    if (a0 == "history" || a0 == "recent") {
        PrintEndemicHistory(true);
        return;
    }
    if (a0 == "help" || a0 == "?") {
        if (en) {
            g_host->ChatPrint("[endemic] Size: `$rng endemic min|normal|max [0~100]` | `exact 1.2x` | `exact 30cm` | `ec059 max`");
            g_host->ChatPrint("[endemic] Rare: `$rng endemic rare on|0%|off|log|history` | `rare ec058 on` | `rare wintermoon_nettle on` | `ec058 rare on`");
            g_host->ChatPrint("[endemic] History/Clear: `$rng endemic history` | `clear` | `ec059 clear`");
        } else {
            g_host->ChatPrint(u8"[endemic] 尺寸: `$rng endemic min|normal|max [0~100]` | `exact 1.2x` | `exact 30cm` | `ec059 max`");
            g_host->ChatPrint(u8"[endemic] 稀有: `$rng endemic rare on|0%|off|log|history` | `rare ec058 on` | `rare 月羽天母 on` | `ec058 rare on`");
            g_host->ChatPrint(u8"[endemic] 记录/清理: `$rng endemic history` (查刷出代号与cm) | `clear` | `ec059 clear`");
        }
        return;
    }
    if (a0 == "off" || a0 == "clear" || a0 == "reset") {
        if (argc >= 2) {
            std::uint32_t aid = 0, atp = kAnyAnimalType;
            if (ParseSpeciesSelector(argv[1], aid, atp)) {
                {
                    std::lock_guard<std::mutex> lk(g_stateMx);
                    g_speciesPolicies.erase(
                        std::remove_if(g_speciesPolicies.begin(), g_speciesPolicies.end(),
                                       [&](const EndemicPolicy& p) { return p.animalId == aid && p.animalType == atp; }),
                        g_speciesPolicies.end());
                    RemoveSpeciesRareRuleLocked(aid, atp);
                }
                g_host->ChatPrintf(en ? "[endemic] Removed rules for %s" : u8"[endemic] 已移除 %s 的专属规则",
                                   FormatSpeciesCode(aid, atp).c_str());
                return;
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            g_globalPolicy     = EndemicPolicy{};
            g_speciesPolicies.clear();
            g_globalRarePolicy = RarePolicy{};
            g_speciesRarePolicies.clear();
        }
        g_host->ClearSlotAllConditions(kRvaCrownTierRoll);
        g_host->ClearSlotAllConditions(kRvaFieldRangeStd);
        g_host->ClearSlotAllConditions(kRvaFieldRangeSpec);
        g_host->ClearSlotAllConditions(kRvaCaptureIntraTierRoll);
        ClearRareSlotConditions();
        g_host->ChatPrint(en ? "[endemic] Cleared all endemic size & rare rules -> vanilla RNG"
                             : u8"[endemic] 已清空全部环境生物尺寸与稀有生成规则 -> 恢复正常随机");
        return;
    }

    std::uint32_t aid = 0, atp = kAnyAnimalType;
    if (ParseSpeciesSelector(argv[0], aid, atp)) {
        const std::string code = FormatSpeciesCode(aid, atp);
        if (argc < 2) {
            g_host->ChatPrintf(en ? "[endemic] Usage: `$rng endemic %s <min|normal|max|exact 1.2x|rare [on|0%|off]|clear>`"
                                  : u8"[endemic] 用法: `$rng endemic %s <min|normal|max|exact 1.2x|rare [on|0%|off]|clear>`",
                               argv[0]);
            return;
        }
        const std::string sub = LowerAscii(argv[1]);
        if (sub == "rare") {
            if (argc == 2) {
                ApplySpeciesRareRule(aid, atp, RareMode::kForceOn, false);
                return;
            }
            RareMode mode = RareMode::kVanilla;
            bool isClear = false;
            if (!ParseRareModeToken(LowerAscii(argv[2]), mode, isClear) || argc > 3) {
                g_host->ChatPrint(en ? "[endemic/rare] Invalid mode; use `on` / `0%` / `block` / `off`"
                                     : u8"[endemic/rare] 参数无效；支持 `on` / `0%` / `block` / `off`");
                return;
            }
            ApplySpeciesRareRule(aid, atp, mode, isClear);
            return;
        }
        if (sub == "clear" || sub == "off" || sub == "reset") {
            {
                std::lock_guard<std::mutex> lk(g_stateMx);
                g_speciesPolicies.erase(
                    std::remove_if(g_speciesPolicies.begin(), g_speciesPolicies.end(),
                                   [&](const EndemicPolicy& p) { return p.animalId == aid && p.animalType == atp; }),
                    g_speciesPolicies.end());
                RemoveSpeciesRareRuleLocked(aid, atp);
            }
            g_host->ChatPrintf(en ? "[endemic] Removed rules for %s" : u8"[endemic] 已移除 %s 的专属规则", code.c_str());
            return;
        }

        EndemicPolicy pol{};
        const ParseResult pr = ParsePolicyTokens(argc - 1, argv + 1, aid, atp, pol);
        if (pr == ParseResult::kOutOfRange) {
            g_host->ChatPrint(en ? "[endemic] Out of range: intra 0~100, scale 0.3~3.0 (30%~300%), cm > 0"
                                 : u8"[endemic] 数值超出范围: 档内比例 `0~100`，倍率 `0.3~3.0` 或 `30%~300%`，厘米 `>0`");
            return;
        }
        if (pr != ParseResult::kOk) {
            g_host->ChatPrint(en ? "[endemic] Invalid args; use min / normal / max / exact <val> / rare / clear"
                                 : u8"[endemic] 参数无效；支持 `min` / `normal` / `max` / `exact <值>` / `rare` / `clear`");
            return;
        }

        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            bool updated = false;
            for (auto& existing : g_speciesPolicies) {
                if (existing.animalId == aid && existing.animalType == atp) {
                    existing = pol;
                    updated  = true;
                    break;
                }
            }
            if (!updated) g_speciesPolicies.push_back(pol);
        }
        g_host->ChatPrintf(en ? "[endemic] Set %s -> %s (other species unaffected)"
                              : u8"[endemic] 已设专属 %s -> %s (其它环境生物不受影响)",
                           code.c_str(), FormatPolicySummary(pol, en).c_str());
        return;
    }

    EndemicPolicy pol{};
    const ParseResult pr = ParsePolicyTokens(argc, argv, 0U, kAnyAnimalType, pol);
    if (pr == ParseResult::kReversedOrderHint) {
        g_host->ChatPrintf(en ? "[endemic] Order hint: put species first, e.g. `$rng endemic %s %s`"
                              : u8"[endemic] 顺序提示: 按物种锁定请把代号放前面，例如 `$rng endemic %s %s`",
                           argv[argc - 1], argv[0]);
        return;
    }
    if (pr == ParseResult::kOutOfRange) {
        g_host->ChatPrint(en ? "[endemic] Out of range: intra 0~100, scale 0.3~3.0 (30%~300%), cm > 0"
                             : u8"[endemic] 数值超出范围: 档内比例 `0~100`，倍率 `0.3~3.0` 或 `30%~300%`，厘米 `>0`");
        return;
    }
    if (pr != ParseResult::kOk) {
        g_host->ChatPrintf(en ? "[endemic] Unknown arg `%s`; type `$rng endemic help`"
                              : u8"[endemic] 未知参数 `%s`；输 `$rng endemic help` 查看用法",
                           argv[0]);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        g_globalPolicy = pol;
    }
    g_host->ChatPrintf(en ? "[endemic] Global endemic rule -> %s" : u8"[endemic] 已设全局环境生物尺寸规则 -> %s",
                       FormatPolicySummary(pol, en).c_str());
}

void __fastcall Complete_Endemic(const char*,
                                 int argc,
                                 const char* const* argv,
                                 const char* prefix,
                                 MhwRngCompletionSinkFn addCand,
                                 void* sinkCtx,
                                 void*) {
    if (!addCand) return;
    std::vector<std::string> knownSpecies;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        for (const auto& p : g_speciesPolicies) {
            if (p.animalId != 0U) knownSpecies.push_back(FormatSpeciesCodeRaw(p.animalId, p.animalType));
        }
        for (const auto& p : g_speciesRarePolicies) {
            if (p.animalId != 0U) knownSpecies.push_back(FormatSpeciesCodeRaw(p.animalId, p.animalType));
        }
        for (std::size_t i = 0; i < g_historyCount; ++i) {
            if (g_history[i].validSpecies && g_history[i].animalId != 0U) {
                knownSpecies.push_back(FormatSpeciesCodeRaw(g_history[i].animalId, kAnyAnimalType));
                knownSpecies.push_back(FormatSpeciesCodeRaw(g_history[i].animalId, g_history[i].animalType));
            }
        }
        for (std::size_t i = 0; i < g_rareHistoryCount; ++i) {
            if (g_rareHistory[i].validSp && g_rareHistory[i].animalId != 0U) {
                knownSpecies.push_back(FormatSpeciesCodeRaw(g_rareHistory[i].animalId, kAnyAnimalType));
                knownSpecies.push_back(FormatSpeciesCodeRaw(g_rareHistory[i].animalId, g_rareHistory[i].animalType));
            }
        }
    }

    // Game-data names:
    //   - Empty prefix (`prefixEmpty`): lists species names in the active UI language (`e.zh` in `zh`, `e.slug`
    //     in `en`) in BOTH `rare` contexts (~40 rare + Great-size-fish entries) AND general contexts (all 92
    //     entries, e.g. `$rng endemic <Tab>` and `$rng endemic clear <Tab>`).
    //   - Single ASCII character prefix (`!prefixEmpty && !typedSomething`) in non-`rare` contexts: omits the
    //     92 species names so short keyword completions (`endemic r<Tab>` -> `endemic rare `) stay unambiguous.
    //   - CJK prefix (`>= 0x80`) or `>= 2` ASCII characters (`typedSomething`): offers `e.zh` + `e.slug` in `zh`,
    //     or `e.slug` in `en`.
    const bool prefixEmpty = (!prefix || prefix[0] == '\0');
    const bool typedSomething =
        (!prefixEmpty &&
         (static_cast<unsigned char>(prefix[0]) >= 0x80U || std::strlen(prefix) >= 2U));
    const bool en = IsEn();
    auto addNames = [&](bool rareOnly) {
        if (!rareOnly && !prefixEmpty && !typedSomething) return;
        for (const auto& e : kEndemicNames) {
            if (rareOnly && e.isRare != 1U && e.hasGreat == 0U) continue;
            if (en) {
                addCand(e.slug, sinkCtx);
            } else {
                addCand(e.zh, sinkCtx);
                if (!prefixEmpty) addCand(e.slug, sinkCtx);
            }
        }
    };

    if (argc == 0) {
        static const char* const kTop[] = {
            "status", "history", "help", "clear", "min", "normal", "max", "exact", "rare"
        };
        for (const char* t : kTop) addCand(t, sinkCtx);
        for (const auto& s : knownSpecies) addCand(s.c_str(), sinkCtx);
        addNames(false);
        return;
    }
    if (argc == 1 && argv && argv[0]) {
        const std::string a0 = LowerAscii(argv[0]);
        if (a0 == "rare") {
            static const char* const kRareTop[] = {
                "on", "off", "0%", "block", "clear", "status", "history", "log", "help"
            };
            for (const char* t : kRareTop) addCand(t, sinkCtx);
            for (const auto& s : knownSpecies) addCand(s.c_str(), sinkCtx);
            addNames(true);
            return;
        }
        if (a0 == "clear" || a0 == "off" || a0 == "reset") {
            for (const auto& s : knownSpecies) addCand(s.c_str(), sinkCtx);
            addNames(false);
            return;
        }
        const bool isStandaloneGlobal =
            (a0 == "status" || a0 == "info" || a0 == "history" || a0 == "recent" ||
             a0 == "help" || a0 == "?" ||
             a0 == "min" || a0 == "mini" || a0 == "small" ||
             a0 == "normal" || a0 == "mid" || a0 == "std" ||
             a0 == "max" || a0 == "giant" || a0 == "large" || a0 == "big" ||
             a0 == "exact");
        if (!isStandaloneGlobal) {
            std::uint32_t aid = 0U, atp = kAnyAnimalType;
            if (ParseSpeciesSelector(argv[0], aid, atp)) {
                static const char* const kActions[] = { "min", "normal", "max", "exact", "rare", "clear" };
                for (const char* a : kActions) addCand(a, sinkCtx);
            }
        }
        return;
    }
    if (argc == 2 && argv && argv[0] && argv[1]) {
        const std::string a0 = LowerAscii(argv[0]);
        const std::string a1 = LowerAscii(argv[1]);
        static const char* const kRareModes[] = { "on", "0%", "block", "off", "clear" };
        if (a0 == "rare") {
            if (a1 == "log") {
                addCand("on", sinkCtx);
                addCand("off", sinkCtx);
                return;
            }
            if (a1 == "clear" || a1 == "off" || a1 == "reset") {
                for (const auto& s : knownSpecies) addCand(s.c_str(), sinkCtx);
                addNames(true);
                return;
            }
            std::uint32_t aid = 0U, atp = kAnyAnimalType;
            if (ParseSpeciesSelector(argv[1], aid, atp)) {
                for (const char* m : kRareModes) addCand(m, sinkCtx);
            }
            return;
        }
        std::uint32_t aid = 0U, atp = kAnyAnimalType;
        if (a1 == "rare" && ParseSpeciesSelector(argv[0], aid, atp)) {
            for (const char* m : kRareModes) addCand(m, sinkCtx);
        }
    }
}

const MhwRngPluginDescriptor kPluginDesc = {
    kMhwRngApiVersion,
    "endemic",
    u8"环境生物尺寸与稀有生成控制插件",
    "1.3.0",
    u8"控制环境生物大小金档位、精确cm尺寸以及稀有环境生物/大型个体鱼/地区稀有毛吉纯RNG刷出"
};

} // namespace

MHW_RNG_PLUGIN_EXPORT const MhwRngPluginDescriptor* MhwRngPlugin_GetInfo() {
    return &kPluginDesc;
}

MHW_RNG_PLUGIN_EXPORT bool MhwRngPlugin_Init(const MhwRngHostApi* host, MhwRngPluginId selfId) {
    if (!host || host->apiVersion != kMhwRngApiVersion) return false;
    g_host   = host;
    g_selfId = selfId;

    MhwRng_RegisterPluginInfoBi(
        host, selfId,
        u8"环境生物尺寸与稀有生成控制插件",
        "Endemic Life Size, Crown & Rare Controller",
        u8"控制环境生物大小金档位、精确cm尺寸与稀有生物纯RNG生成",
        "Endemic life crown tier, exact size & pure-RNG rare spawn controller");

    MhwRng_RegisterSiteMetaBi(
        host, selfId,
        kRvaCrownTierRoll,
        "endemic.crown",
        u8"环境生物大小金档位抽取 (由 endemic 插件托管)",
        "Endemic life crown tier roll (managed by endemic plugin)",
        &FormatEndemicSlotStatus,
        nullptr);
    MhwRng_RegisterSiteMetaBi(
        host, selfId,
        kRvaFieldRangeStd,
        "endemic.field_size",
        u8"环境生物地图刷新档内尺寸插值 (经 0x0672FBC 自动展开)",
        "Endemic field spawn intra-tier size roll (via 0x0672FBC)",
        &FormatEndemicSlotStatus,
        nullptr);
    MhwRng_RegisterSiteMetaBi(
        host, selfId,
        kRvaFieldRangeSpec,
        "endemic.special_size",
        u8"环境生物特殊区域刷新档内尺寸插值 (经 0x0672FBC 自动展开)",
        "Endemic special-zone spawn intra-tier size roll (via 0x0672FBC)",
        &FormatEndemicSlotStatus,
        nullptr);
    MhwRng_RegisterSiteMetaBi(
        host, selfId,
        kRvaCaptureIntraTierRoll,
        "endemic.cap_size",
        u8"环境生物捕获时档内精确尺寸抽取 (由 endemic 插件托管)",
        "Endemic capture intra-tier exact size roll (managed by endemic)",
        &FormatEndemicSlotStatus,
        nullptr);

    MhwRng_RegisterSiteMetaBi(
        host, selfId,
        kRvaRareMasterRoll,
        "endemic.rare",
        u8"稀有环境生物、额外大型个体鱼与毛吉洞刷新抽签 (由 endemic 插件托管)",
        "Master rare endemic, Great-Size fish & Moly burrow spawn roll (managed by endemic)",
        &FormatEndemicRareSlotStatus,
        nullptr);
    MhwRng_RegisterSiteMetaBi(
        host, selfId,
        kRvaRareMolyRoll,
        "endemic.rare_moly",
        u8"聚魔之地地区稀有毛吉形态抽签 (由 endemic 插件托管)",
        "Guiding Lands regional rare Moly variant roll (managed by endemic)",
        &FormatEndemicRareSlotStatus,
        nullptr);
    MhwRng_RegisterSiteMetaBi(
        host, selfId,
        kRvaRareGroupRoll,
        "endemic.rare_group",
        u8"族群级稀有生物可用性抽签 (由 endemic 插件托管)",
        "Endemic group-level rare availability roll (managed by endemic)",
        &FormatEndemicRareSlotStatus,
        nullptr);
    MhwRng_RegisterSiteMetaBi(
        host, selfId,
        kRvaRareMutexTieBreak,
        "endemic.rare_mutex",
        u8"互斥稀有组多选一保底抽签 (防止空槽抹除已刷出的稀有生物)",
        "Mutually-exclusive rare group tie-breaker (protects forced rare spawns)",
        &FormatEndemicRareSlotStatus,
        nullptr);

    auto registerPrePost = [&](std::uint32_t rva, MhwRngSlotHookFn preFn, MhwRngSlotHookFn postFn) {
        MhwRngHookDesc h{};
        h.structSize = sizeof(h);
        h.rva        = rva;
        h.priority   = 0;
        h.flags      = kMhwRngHookPre | kMhwRngHookPost | kMhwRngHookObserveOnly;
        h.preFn      = preFn;
        h.postFn     = postFn;
        host->RegisterSlotHookEx(selfId, &h);
    };

    registerPrePost(kRvaCrownTierRoll, &OnEndemicCrownTierDrawPre, &OnEndemicCrownTierDrawPost);
    host->RegisterSlotHook(selfId, kRvaFieldRangeStd,        &OnFieldStdIntraTierDraw,  nullptr);
    host->RegisterSlotHook(selfId, kRvaFieldRangeSpec,       &OnFieldSpecIntraTierDraw, nullptr);
    host->RegisterSlotHook(selfId, kRvaCaptureIntraTierRoll, &OnCaptureIntraTierDraw,   nullptr);

    registerPrePost(kRvaRareMasterRoll,    &OnRareMasterDrawPre, &OnRareMasterDrawPost);
    registerPrePost(kRvaRareMolyRoll,      &OnRareMolyDrawPre,   &OnRareMolyDrawPost);
    registerPrePost(kRvaRareGroupRoll,     &OnRareGroupDrawPre,  &OnRareGroupDrawPost);
    registerPrePost(kRvaRareMutexTieBreak, &OnRareMutexDrawPre,  &OnRareMutexDrawPost);

    MhwRng_RegisterSubcommandBi(
        host, selfId,
        "endemic",
        "$rng endemic [ecXXX[_YY]] <min|normal|max|exact 1.2x|rare [on|0%|off]|clear>",
        u8"环境生物尺寸大小金与稀有生物纯RNG控制",
        "Control endemic life crown tier, exact size & pure-RNG rare spawns",
        &Cmd_Endemic,
        nullptr);
    MhwRng_RegisterSubcommandCompleter(host, selfId, "endemic", &Complete_Endemic, nullptr);
    g_worldEvHandle = MhwRng_HostSubscribeEvent(
        host, selfId, "mhw.world.*",
        [](const MhwRngCustomEvent* ev, void*) {
            if (!ev || !ev->topic) return;
            if (std::strcmp(ev->topic, kMhwWorldEvTitleReturn) == 0) {
                std::lock_guard<std::mutex> lk(g_stateMx);
                g_historyCount     = 0U;
                g_rareHistoryCount = 0U;
                return;
            }
            if (std::strcmp(ev->topic, kMhwWorldEvHourChanged) == 0 ||
                std::strcmp(ev->topic, kMhwWorldEvWeatherChanged) == 0) {
                bool                 logOn    = false;
                const MhwRngHostApi* hostCopy = nullptr;
                {
                    std::lock_guard<std::mutex> lk(g_stateMx);
                    logOn    = g_rareLogEnabled;
                    hostCopy = g_host;
                }
                if (logOn && hostCopy && ev->payload && ev->payloadSize >= sizeof(MhwWorldEventPayload)) {
                    const auto* wp = static_cast<const MhwWorldEventPayload*>(ev->payload);
                    const bool  en = MhwRng_HostIsEnglish(hostCopy);
                    if (std::strcmp(ev->topic, kMhwWorldEvHourChanged) == 0) {
                        if (hostCopy->ChatPrintfAsync) {
                            hostCopy->ChatPrintfAsync(
                                en ? "[endemic/rare] World hour -> %02dh (hourly rare checks active)"
                                   : u8"[endemic/rare] 世界时间切换 -> %02dh (整点稀有生物判定)",
                                wp->curr.gameHourInt);
                        }
                    } else {
                        if (hostCopy->ChatPrintfAsync) {
                            hostCopy->ChatPrintfAsync(
                                en ? "[endemic/rare] Weather -> id=%u (%s)"
                                   : u8"[endemic/rare] 天气变化 -> id=%u (%s)",
                                wp->curr.weatherId,
                                wp->curr.isSpecialWeather ? (en ? "Special" : u8"特殊天气")
                                                          : (en ? "Clear" : u8"常规"));
                        }
                    }
                }
            }
        },
        nullptr);
    return true;
}

MHW_RNG_PLUGIN_EXPORT void MhwRngPlugin_Shutdown() {
    const MhwRngHostApi* hostCopy = nullptr;
    MhwRngPluginId       selfCopy = 0U;
    MhwRngHookHandle     evCopy   = kMhwRngInvalidHandle;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        hostCopy           = g_host;
        selfCopy           = g_selfId;
        evCopy             = g_worldEvHandle;
        g_worldEvHandle    = kMhwRngInvalidHandle;
        g_globalPolicy     = EndemicPolicy{};
        g_speciesPolicies.clear();
        g_historyCount     = 0U;
        g_globalRarePolicy = RarePolicy{};
        g_speciesRarePolicies.clear();
        g_rareLogEnabled   = false;
        g_rareHistoryCount = 0U;
        g_host             = nullptr;
        g_selfId           = 0U;
    }
    MhwRng_HostUnsubscribeEvent(hostCopy, selfCopy, evCopy);
}

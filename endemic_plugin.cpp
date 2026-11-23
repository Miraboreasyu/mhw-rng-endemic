// ============================================================================
//  MHW RNG Official Plugin: Endemic Life Size & Crown Controller (`rng_endemic.dll`)
//  Version : 1.1.0 (SDK v3.1)
//  Targets :
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
//
//  Architecture & SDK Best-Practice Highlights:
//    - Zero extra game hooks: operates purely via microkernel `RegisterSlotHookEx`.
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

// Verified caller return RVAs for stack-frame validation in `FUN_14166ccc0`:
constexpr std::uintptr_t kRvaCallerCaptureRollRet  = 0x1669C19U; // `0x141669AC0` -> `CALL 0x14166ccc0`
constexpr std::uintptr_t kRvaCallerFieldWrapLo     = 0x0271840U; // `0x140271840` (`nAnimal::SizeSetData::ctor`) start
constexpr std::uintptr_t kRvaCallerFieldWrapHi     = 0x0271960U; // `0x140271840` end
constexpr std::uintptr_t kRvaCallerFieldTailRet    = 0x1D2A407U; // `0x141D2A1E0` -> `CALL 0x140271840` (tail-jmp to 0x14166CCC0)

// Verified `rAnimalSize` (`DAT_145012740`) & `uAnimal` offsets:
constexpr std::uintptr_t kRvaAnimalSizeGlobalPtr   = 0x5012740U;
constexpr std::uintptr_t kOffUAnimalId             = 0x1AF0U;
constexpr std::uintptr_t kOffUAnimalType           = 0x1AF4U;

constexpr float          kProbEps                  = 1.192092896e-7f; // `DAT_142e4fe48` (`FLT_EPSILON`)
constexpr std::uint32_t  kMinSafeFloatU32          = 0x00000001U;
constexpr std::uint32_t  kMaxSafeFloatU32          = 0xFFFFFF00U;
constexpr std::uint32_t  kAnyAnimalType            = 0xFFFFFFFFU;

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

constexpr std::size_t kHistoryCap = 6U;

const MhwRngHostApi*       g_host   = nullptr;
MhwRngPluginId             g_selfId = 0U;

// Strict leaf mutex: NEVER call `g_host->...` while holding `g_stateMx`!
std::mutex                 g_stateMx;
EndemicPolicy              g_globalPolicy{};
std::vector<EndemicPolicy> g_speciesPolicies;
EndemicHistoryEntry        g_history[kHistoryCap]{};
std::size_t                g_historyCount = 0U;

thread_local ThreadLocalEndemicCtx tls_lastRoll{};

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

bool IsValidUserPtr(std::uintptr_t p, std::uintptr_t alignMask = 0x7ULL) noexcept {
    return p >= 0x100000ULL && p <= 0x00007FFFFFFF0000ULL && (p & alignMask) == 0ULL;
}

std::uint32_t MakeSpeciesCondId(std::uint32_t animalId, std::uint32_t animalType) noexcept {
    if (animalId == 0U) return 0U;
    if (animalType == kAnyAnimalType) {
        return (animalId << 8) | 0xFFU;
    }
    return (animalId << 8) | (animalType & 0x7FU);
}

std::string FormatSpeciesCode(std::uint32_t animalId, std::uint32_t animalType) {
    char buf[32]{};
    if (animalType == kAnyAnimalType) {
        std::snprintf(buf, sizeof(buf), "ec%03u*", animalId);
    } else {
        std::snprintf(buf, sizeof(buf), "ec%03u_%02u", animalId, animalType);
    }
    return buf;
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

// Parses species selectors:
//   - `ec059` / `ec59` / `#59`           -> (animalId=59, animalType=kAnyAnimalType)
//   - `ec059_02` / `ec59_2` / `59:2`     -> (animalId=59, animalType=2)
bool ParseSpeciesSelector(const std::string& rawTok,
                          std::uint32_t& outAnimalId,
                          std::uint32_t& outAnimalType) {
    if (rawTok.empty()) return false;
    const std::string low = LowerAscii(rawTok);

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

std::uint32_t FractionToTierU32(double frac0To1, bool allowExactOne) noexcept {
    if (!(frac0To1 > 0.0)) return 0U;
    if (frac0To1 >= 1.0) return allowExactOne ? 0xFFFFFFFFU : kMaxSafeFloatU32;
    const auto u = static_cast<std::uint64_t>(frac0To1 * static_cast<double>(kMaxSafeFloatU32) + 0.5);
    if (u >= kMaxSafeFloatU32) return kMaxSafeFloatU32;
    return static_cast<std::uint32_t>(u);
}

std::uint32_t FractionToOpenSafeU32(double frac0To1) noexcept {
    if (!(frac0To1 > 0.0)) return kMinSafeFloatU32;
    if (frac0To1 >= 1.0) return kMaxSafeFloatU32;
    const auto u = static_cast<std::uint64_t>(frac0To1 * static_cast<double>(kMaxSafeFloatU32) + 0.5);
    if (u <= kMinSafeFloatU32) return kMinSafeFloatU32;
    if (u >= kMaxSafeFloatU32) return kMaxSafeFloatU32;
    return static_cast<std::uint32_t>(u);
}

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
const EndemicPolicy* FindActivePolicyLocked(bool validSpecies,
                                            std::uint32_t animalId,
                                            std::uint32_t animalType) noexcept {
    if (validSpecies) {
        // 1. Exact `(animalId, animalType)` match
        for (const auto& p : g_speciesPolicies) {
            if (p.mode != PolicyMode::kNone && p.animalId == animalId && p.animalType == animalType) {
                return &p;
            }
        }
        // 2. Family `(animalId, *)` match
        for (const auto& p : g_speciesPolicies) {
            if (p.mode != PolicyMode::kNone && p.animalId == animalId && p.animalType == kAnyAnimalType) {
                return &p;
            }
        }
    }
    return &g_globalPolicy;
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

void PrintEndemicStatus() {
    const bool en = IsEn();
    EndemicPolicy              globalSnap{};
    std::vector<EndemicPolicy> spSnaps;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        globalSnap = g_globalPolicy;
        spSnaps    = g_speciesPolicies;
    }

    g_host->ChatPrintf(en ? "[endemic] Global: %s | Per-species rules: %zu"
                          : u8"[endemic] 全局策略: %s | 按物种专属规则: %zu 条",
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
            g_host->ChatPrint("[endemic] Global: `$rng endemic min|normal|max [0~100]` | `exact 1.2x` | `exact 30cm`");
            g_host->ChatPrint("[endemic] Species: `$rng endemic ec059 max` (all variants) or `$rng endemic ec059_02 min`");
            g_host->ChatPrint("[endemic] History/Clear: `$rng endemic history` | `clear` | `ec059 clear`");
        } else {
            g_host->ChatPrint(u8"[endemic] 全局: `$rng endemic min|normal|max [0~100]` | `exact 1.2x` | `exact 30cm`");
            g_host->ChatPrint(u8"[endemic] 按物种: `$rng endemic ec059 max` (全亚种) 或 `$rng endemic ec059_02 min` (单变种)");
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
                }
                g_host->ChatPrintf(en ? "[endemic] Removed rule for %s" : u8"[endemic] 已移除 %s 的专属尺寸规则",
                                   FormatSpeciesCode(aid, atp).c_str());
                return;
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            g_globalPolicy = EndemicPolicy{};
            g_speciesPolicies.clear();
        }
        g_host->ClearSlotAllConditions(kRvaCrownTierRoll);
        g_host->ClearSlotAllConditions(kRvaFieldRangeStd);
        g_host->ClearSlotAllConditions(kRvaFieldRangeSpec);
        g_host->ClearSlotAllConditions(kRvaCaptureIntraTierRoll);
        g_host->ChatPrint(en ? "[endemic] Cleared all endemic size rules -> vanilla RNG"
                             : u8"[endemic] 已清空全部环境生物尺寸规则 -> 恢复正常随机");
        return;
    }

    std::uint32_t aid = 0, atp = kAnyAnimalType;
    if (ParseSpeciesSelector(argv[0], aid, atp)) {
        const std::string code = FormatSpeciesCode(aid, atp);
        if (argc < 2) {
            g_host->ChatPrintf(en ? "[endemic] Usage: `$rng endemic %s <min|normal|max|exact 1.2x|clear>`"
                                  : u8"[endemic] 用法: `$rng endemic %s <min|normal|max|exact 1.2x|clear>`",
                               argv[0]);
            return;
        }
        const std::string sub = LowerAscii(argv[1]);
        if (sub == "clear" || sub == "off" || sub == "reset") {
            {
                std::lock_guard<std::mutex> lk(g_stateMx);
                g_speciesPolicies.erase(
                    std::remove_if(g_speciesPolicies.begin(), g_speciesPolicies.end(),
                                   [&](const EndemicPolicy& p) { return p.animalId == aid && p.animalType == atp; }),
                    g_speciesPolicies.end());
            }
            g_host->ChatPrintf(en ? "[endemic] Removed rule for %s" : u8"[endemic] 已移除 %s 的专属尺寸规则", code.c_str());
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
            g_host->ChatPrint(en ? "[endemic] Invalid args; use min / normal / max / exact <val> / clear"
                                 : u8"[endemic] 参数无效；支持 `min` / `normal` / `max` / `exact <值>` / `clear`");
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
                                 const char*,
                                 MhwRngCompletionSinkFn addCand,
                                 void* sinkCtx,
                                 void*) {
    if (!addCand) return;
    std::vector<std::string> knownSpecies;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        for (const auto& p : g_speciesPolicies) {
            if (p.animalId != 0U) {
                knownSpecies.push_back(FormatSpeciesCode(p.animalId, p.animalType));
            }
        }
        for (std::size_t i = 0; i < g_historyCount; ++i) {
            if (g_history[i].validSpecies && g_history[i].animalId != 0U) {
                knownSpecies.push_back(FormatSpeciesCode(g_history[i].animalId, kAnyAnimalType));
                knownSpecies.push_back(FormatSpeciesCode(g_history[i].animalId, g_history[i].animalType));
            }
        }
    }

    if (argc == 0) {
        static const char* const kTop[] = {
            "status", "history", "help", "clear", "min", "normal", "max", "exact"
        };
        for (const char* t : kTop) addCand(t, sinkCtx);
        for (const auto& s : knownSpecies) addCand(s.c_str(), sinkCtx);
        return;
    }
    if (argc == 1 && argv && argv[0]) {
        const std::string a0 = LowerAscii(argv[0]);
        if (a0 == "clear" || a0 == "off" || a0 == "reset") {
            for (const auto& s : knownSpecies) addCand(s.c_str(), sinkCtx);
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
                static const char* const kActions[] = { "min", "normal", "max", "exact", "clear" };
                for (const char* a : kActions) addCand(a, sinkCtx);
            }
        }
    }
}

const MhwRngPluginDescriptor kPluginDesc = {
    kMhwRngApiVersion,
    "endemic",
    u8"环境生物尺寸与大小金控制插件",
    "1.2.0",
    u8"控制环境生物大小金档位与档内尺寸(直接展开0x0672FBC包装并支持多语言与精确cm反算)"
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
        u8"环境生物尺寸与大小金控制插件",
        "Endemic Life Size & Crown Controller",
        u8"控制环境生物大小金档位与精确cm/倍率尺寸",
        "Endemic life crown tier & exact scale/cm size controller");

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

    MhwRngHookDesc hCrown{};
    hCrown.structSize = sizeof(hCrown);
    hCrown.rva        = kRvaCrownTierRoll;
    hCrown.priority   = 0;
    hCrown.flags      = kMhwRngHookPre | kMhwRngHookPost | kMhwRngHookObserveOnly;
    hCrown.preFn      = &OnEndemicCrownTierDrawPre;
    hCrown.postFn     = &OnEndemicCrownTierDrawPost;
    host->RegisterSlotHookEx(selfId, &hCrown);

    host->RegisterSlotHook(selfId, kRvaFieldRangeStd,        &OnFieldStdIntraTierDraw,  nullptr);
    host->RegisterSlotHook(selfId, kRvaFieldRangeSpec,       &OnFieldSpecIntraTierDraw, nullptr);
    host->RegisterSlotHook(selfId, kRvaCaptureIntraTierRoll, &OnCaptureIntraTierDraw,   nullptr);

    MhwRng_RegisterSubcommandBi(
        host, selfId,
        "endemic",
        "$rng endemic [ecXXX[_YY]] <min|normal|max|exact 1.2x|clear>",
        u8"环境生物尺寸与大小金控制(覆盖刷新与捕获抽签)",
        "Control endemic life crown tier & exact scale/cm on spawn and capture",
        &Cmd_Endemic,
        nullptr);
    MhwRng_RegisterSubcommandCompleter(host, selfId, "endemic", &Complete_Endemic, nullptr);
    return true;
}

MHW_RNG_PLUGIN_EXPORT void MhwRngPlugin_Shutdown() {
    std::lock_guard<std::mutex> lk(g_stateMx);
    g_globalPolicy = EndemicPolicy{};
    g_speciesPolicies.clear();
    g_historyCount = 0U;
    g_host         = nullptr;
    g_selfId       = 0U;
}

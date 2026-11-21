# `rng_endemic` 官方插件技术文档（环境生物尺寸与大小金控制）

- **插件标识**：`endemic`（编译产物 `rng_endemic.dll`，版本 `1.0.0`）
- **源码位置**：[endemic_plugin.cpp](endemic_plugin.cpp)
- **宿主依赖**：`mhw_rng.dll`（Plugin SDK `kMhwRngApiVersion == 2`，头文件 [mhw_rng_api.h](include/mhw_rng_api.h)）
- **接管调用点**：`0x166CFE1`（档位抽取）、`0x0672FBC`（地图刷新档内插值，仅过滤环境生物调用来源）、`0x1669CB6`（捕获时档内插值）

本文档说明 `rng_endemic` 插件所依据的原版环境生物尺寸逆向逻辑、共享随机函数的调用栈过滤机制、按物种条件分流方式以及聊天命令参考。

---

## 一、游戏原版环境生物尺寸计算逻辑

与大型怪物的单次 38 档查表不同，《怪物猎人：世界 冰原》15.23 版本中环境生物的尺寸采用**两阶段抽签**：第一阶段抽取大小金档位（`0 = 小金`、`1 = 普通`、`2 = 大金`），第二阶段在对应档位的 `[tierMin, tierMax]` 连续倍率区间内做线性插值，最后乘以该物种的基础厘米数 `baseSizeCm`。

### 1. 两条完整调用路径
1. **地图刷新路径（常规环境生物、鱼类等在场上生成时定型）**：
   - `FUN_141d2a1e0(uAnimal* rcx)`（`0x141D2A1E0`）从 `uAnimal + 0x1AF0` 读取 `animalId`，从 `+0x1AF4` 读取变种编号 `animalType`。
   - 调用 `0x140271840` $\rightarrow$ `FUN_14166ccc0`（**调用点 `RVA 0x166CFE1`**）抽取大小金档位 `tier`（`0 / 1 / 2`）。
   - 分别调用 `FUN_14166b8c0` 与 `FUN_14166b7f0` 读取 `rAnimalSize`（`0x145012740`）中该物种在 `tier` 档对应的倍率下界与上界 `[minScale, maxScale]`。
   - 调用共享浮点区间随机函数 `FUN_140672f90(minScale, maxScale)`（**调用点 `RVA 0x0672FBC`**，调用返回地址为 `0x1D2A472`，特殊地区路径为 `0x1D2A3CC`），将结果截断保留两位小数写入 `*(float*)(uAnimal + 0x11BC)`。
2. **捕获时重抽路径（`sizeMode == 3` 的特殊物种，捕获瞬间才决定尺寸）**：
   - 捕获结算函数 `FUN_141d28c00`（`0x141D28C00`）先检查 `FUN_14166c800(animalId, animalType)`；若返回非零，则调用 `0x141669AC0(animalId, animalType, &outScale)`。
   - `0x141669AC0` 同样先调用 `FUN_14166ccc0`（**同一个调用点 `RVA 0x166CFE1`**）抽取档位 `tier`，随后在函数内部直接调用 `MtRandom::nrand`（**调用点 `RVA 0x1669CB6`**）完成 `[minScale, maxScale]` 线性插值并四舍五入至两位小数。

### 2. 第一阶段：档位抽取公式（`FUN_14166ccc0`，`RVA 0x166CFE1`）
`FUN_14166ccc0` 在抽签前会综合基础概率 `SizeSetData::mSizeProbability[3]`（偏移 `+0x14 .. +0x1C`）以及猫饭技能（生物博士 / 大小体型猫饭 `0x37`/`0x38`）与悬赏加成（写入栈局部数组 `local_98[3]`）：

```c
uint32_t u = MtRandom::nrand(&MtRandom::sInstance); // retRVA = 0x166CFE1
float roll = (float)u * (1.0f / 4294967296.0f) * totalBase;
float cum = 0.0f;
for (uint32_t t = 0; t < 3; ++t) {
    float baseP = sizeSetData->prob[t];
    if (baseP < FLT_EPSILON) continue;              // 跳过无此大小金档的物种
    float effP = baseP + bonus[t];
    if (effP <= 0.0f) continue;
    cum += effP;
    if (roll <= cum) return t;                      // 0=小金, 1=普通, 2=大金
}
return 1; // 兜底返回普通档
```

### 3. `rAnimalSize` 内存布局（`exeBase + 0x5012740`）

| 结构 | 偏移 | 类型 | 说明 |
| :--- | :--- | :--- | :--- |
| `rAnimalSize*` | `+0xB8` / `+0xC8` | `uint32_t` / `cAnimalSize**` | 表项数量与指针数组基址（`entries[0]` 为默认模板行） |
| `cAnimalSize*` | `+0x08` / `+0x0C` | `int32_t` / `int32_t` | `animalId` 与 `animalType` |
| `cAnimalSize*` | `+0x24` | `float` | `baseSizeCm`（基础厘米尺寸，乘以抽出的倍率即最终厘米数） |
| `cAnimalSize*` | `+0x2C` | `int32_t` | `sizeMode`（`0` 继承默认档位比例，`1` 自定义，`2` 固定 1.0x，`3` 捕获时抽签） |
| `cAnimalSize*` | `+0x30` / `+0x34` | `float` / `float` | 物种全局最小/最大倍率 `minScale`、`maxScale` |
| `ratioRow` | `+0x38, +0x3C, +0x40, +0x44` | `float[4]` | 档位划分比例：`小金=[min, min+span*r0]`、`普通=[min+span*r1, min+span*r2]`、`大金=[min+span*r3, max]` |

---

## 二、插件实现与安全边界设计

### 1. 栈帧上下文溯源与非环境生物调用隔离
- **档位回调（`0x166CFE1`）**：`FUN_14166ccc0` 入栈 7 个寄存器并 `SUB RSP, 0x80`，调用方返回地址固定位于 `slotPtr[24]`。插件校验 `slotPtr[24]` 是否属于捕获调用点（`0x1669C19`）、常规刷新包装（`0x0271840..0x0271960`）或尾跳转路径（`0x1D2A407`），据此安全读取 `(animalId, animalType)`、实时 `SizeSetData::prob[3]` 与栈上的实时猫饭/悬赏加成 `bonus[3]`（`slotPtr + 5`）。
- **实时中点求解**：插件直接利用读出的有效概率 `effProb[0..2]` 计算目标档位累计区间的中点 `(cumBefore + 0.5 * effProb[t]) / totalBase` 换算为安全 `uint32_t`（范围 `[0x00000001U, 0xFFFFFF00U]`）。因此：
  - 即使触发了猫饭大/小体型加成，锁定 `normal`、`mini` 或 `giant` 也不会发生偏移；
  - 若某个物种本身不设小金或大金档（`effProb == 0`），自动回退到最接近的合法档位。
- **共享辅助函数保护（`0x0672FBC`）**：`FUN_140672f90` 是游戏多处共用的浮点区间随机函数。插件在 `OnFieldIntraTierDraw` 入口首先读取 `slotPtr[10]`（因 `FUN_140672f90` 仅执行 `SUB RSP, 0x48`），仅当返回地址精确等于 `exeBase + 0x1D2A472` 或 `exeBase + 0x1D2A3CC` 时才接管，其余调用来源直接返回 `UseCondition(0)`，不影响其它系统。
- **同线程两阶段衔接**：在同一线程上，第二阶段（`0x0672FBC` 或 `0x1669CB6`）紧随第一阶段（`0x166CFE1`）数十条指令后执行。插件通过 `thread_local` 变量将第一阶段识别出的 `(animalId, animalType, chosenTier)` 以及 `exact` 模式算出的精确档内 `uint32_t` 传递给第二阶段，避免重复查表。
- **档内上界截断处理**：地图刷新路径采用 `CVTTSS2SI` 向下截断至两位小数。对于大金档（`tier == 2`）的 `100%` 档内目标，插件注入 `0xFFFFFFFFU`（单精度转换等于 `1.0f`），确保能达到精确的 `maxScale`；对于小金档（`tier == 0`）与普通档（`tier == 1`），上界保持为 `0xFFFFFF00U`（`< 1.0f`），避免越界进入更高一档。

---

## 三、命令参考手册（`$rng endemic`）

| 命令格式 | 作用说明 |
| :--- | :--- |
| `$rng endemic` 或 `$rng endemic status` | 查看全局策略、按物种专属规则列表及最近一次环境生物抽取记录 |
| `$rng endemic history` | 查看最近最多 6 条环境生物刷新/捕获记录（含 `ecXXX_YY` 代号、允许 `cm` 范围与抽出尺寸） |
| `$rng endemic min [0~100]`（别名 `mini` / `small`） | 锁定小金档，可选指定档内位置比例（默认 `0%`，即最小合法尺寸） |
| `$rng endemic normal [0~100]`（别名 `mid` / `std`） | 锁定普通档，可选指定档内位置比例（默认 `50%`） |
| `$rng endemic max [0~100]`（别名 `giant` / `large`） | 锁定大金档，可选指定档内位置比例（默认 `100%`，即最大合法尺寸） |
| `$rng endemic exact <倍率\|百分比\|cm>` | 读取实时 `rAnimalSize` 自动选择对应档位并反算精确倍率或厘米数（例：`exact 1.2x`、`exact 120%`、`exact 25.5cm`） |
| `$rng endemic <物种代号> <策略\|clear>` | 仅对指定环境生物生效，同场其它环境生物保持正常随机 |
| `$rng endemic clear` / `off` / `reset` | 清空全部环境生物尺寸规则，恢复默认随机 |

### 物种代号选择器写法
可使用 `$rng endemic history` 直接查看刚刷出或捕获的环境生物代号，然后按以下格式指定：
- **整类匹配（含所有变种）**：`ec059`、`ec59`、`#59` 或 `ec059*`（例如锁定全部聚魔之地鼹鼠变种）。
- **单变种精确匹配**：`ec059_02`、`ec59_2` 或 `59:2`（仅锁定 `animalId == 59` 且 `animalType == 2` 的个体）。

---

## 四、独立仓库目录、头文件同步与构建

本插件目录自带本地 SDK 头文件副本与独立构建脚本，可作为单独的 Git 仓库维护：

- `endemic_plugin.cpp`：插件主源码
- `include/mhw_rng_api.h`：本地携带的 `mhw_rng` Plugin SDK v2 头文件副本
- `sync_sdk.bat`：从核心框架目录更新 `include\mhw_rng_api.h`
- `build.bat`：自动查找 MSVC x64 工具链并输出 `build\rng_endemic.dll`

常用命令：
```bat
:: 1. 同步最新 SDK 头文件（默认查找 ..\..\include 或 %MHW_RNG_SDK_DIR%）
sync_sdk.bat D:\path\to\mhw-rng

:: 2. 独立编译至 build\rng_endemic.dll
build.bat

:: 3. 编译并直接部署至 Steam 游戏目录 nativePC\plugins\mhw_rng\rng_endemic.dll
build.bat --deploy
```

部署完成后在游戏聊天框输入 `$rng load endemic`（首次加载）或 `$rng reload endemic`（热更新）即可生效。

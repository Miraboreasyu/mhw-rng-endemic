# `rng_endemic` 官方插件技术文档（环境生物尺寸大小金 & 稀有生物生成控制）

- **插件标识**：`endemic`（编译产物 `rng_endemic.dll`，版本 `1.3.0`）
- **源码位置**：[endemic_plugin.cpp](endemic_plugin.cpp)
- **宿主依赖**：`mhw_rng.dll`（Plugin SDK `kMhwRngApiVersion == 3`，头文件 [mhw_rng_api.h](include/mhw_rng_api.h)）
- **接管调用点（共 8 处，全部经 `RegisterSlotHookEx` 零补丁挂钩）**：
  - **尺寸与大小金（4 处）**：`0x166CFE1`（档位抽取）、`0x1D2A472` / `0x1D2A3CC`（经微内核内置 `0x0672FBC` 透明包装展开后的地图刷新档内插值）、`0x1669CB6`（捕获时档内插值）
  - **稀有环境生物生成（4 处）**：`0x1A4ACED`（总稀有生物/稀有鱼/额外大型个体鱼/聚魔毛吉洞抽签）、`0x0271F25`（聚魔之地地区稀有毛吉形态抽签）、`0x166D12E`（族群级稀有开关抽签）、`0x1A44FCC`（互斥稀有组多选一保底抽签）

本文档说明 `rng_endemic` 插件所依据的原版环境生物尺寸与稀有生成逆向逻辑、调用栈隔离、按物种条件分流以及聊天/控制台命令参考。

---

## 一、游戏原版环境生物尺寸计算逻辑

与大型怪物的单次 38 档查表不同，《怪物猎人：世界 冰原》15.23 版本中环境生物的尺寸采用**两阶段抽签**：第一阶段抽取大小金档位（`0 = 小金`、`1 = 普通`、`2 = 大金`），第二阶段在对应档位的 `[tierMin, tierMax]` 连续倍率区间内做线性插值，最后乘以该物种的基础厘米数 `baseSizeCm`。

### 1. 两条完整调用路径
1. **地图刷新路径（常规环境生物、鱼类等在场上生成时定型）**：
   - `FUN_141d2a1e0(uAnimal* rcx)`（`0x141D2A1E0`）从 `uAnimal + 0x1AF0` 读取 `animalId`，从 `+0x1AF4` 读取变种编号 `animalType`。
   - 调用 `0x140271840` → `FUN_14166ccc0`（**调用点 `RVA 0x166CFE1`**）抽取大小金档位 `tier`（`0 / 1 / 2`）。
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

## 二、游戏原版稀有环境生物生成机制与纯 RNG 控制

游戏在**进图初始化**与**每逢游戏内整点切换**时，会在前置条件满足后走 4 处标准 `MtRandom::nrand` 抽签点：

1. **总稀有刷出判定（`FUN_141a4a9b0` @ `RVA 0x1A4ACED`）**：
   - 负责柔毛秧鸡/粗毛秧鸡（`ec015_00` / `ec015_01`）、化石腔棘鱼（`ec017`）、月羽天母（`ec058`）、小蓝歌（`ec055`）、虹色大独角仙（`ec023_02`）、野槌蛇（`ec042`）、温泉金猴（`ec050_01`）、神带鱼（`ec056`）等稀有环境生物，**额外大型个体鱼**（上等粉色异形鱼、帝王旗鱼等）以及**聚魔之地毛吉洞（`ec059`）**激活。
   - **前置非 RNG 门控**：在调用 `nrand` 之前，引擎先校验聚魔地区等级（仅 `ec059`）、目标天气（`RareSetData+0x08`：`0` 任意 / `1` 晴天 / `2` 特殊天气）以及游戏时段（`[mStartTime, mEndTime)`），随后将基础概率与任务、重要委托、猫饭（生物博士/生物研究者）加成求和后放入 `XMM6`。
   - **纯粹判定**：`(float)nrand() * 2^-32 <= XMM6`。插件采取**纯 RNG 模式**，不绕过原版天气/时段/聚魔等级规则：开启强制刷出时返回 `0x00000001U`（$2^{-32}$，对一切 `prob > 0` 必中且跳过 `0.0f` 禁用行）；开启屏蔽时返回 `0xFFFFFFFFU`（`1.0f`，屏蔽一切 `< 100%` 配置）。
2. **聚魔之地地区稀有毛吉判定（`0x140271ce0` @ `RVA 0x0271F25`）**：
   - 毛吉洞激活后单独抽一次普通毛吉（状态 `8`）还是地区稀有毛吉（状态 `7`，需满足地区等级与天气条件；变种由 `animalType` 决定，如 `ec059_02` 掘土毛吉），从 `R15` (`cAnimalGroupCtrlEc059+0x328..0x330`) 读出物种与地区编号。
3. **族群级稀有可用判定（`FUN_14166d070` @ `RVA 0x166D12E`）**：
   - 仅从 `0x140271F90` 调用，通过校验返回地址 `slotPtr[12] == exeBase + 0x0271FFFU` 从 `slotPtr[13]` 安全读出 `cAnimalGroupCtrl*` 并应用同样规则。
4. **互斥稀有组保底抽签（`FUN_141a44f50` @ `RVA 0x1A44FCC`）**：
   - 原版将整个互斥组（包含未刷出的空槽位）统一做 `nrand() % groupCount`，若落到空槽位会把同组刚刷出的稀有生物删掉。
   - 插件在 `RVA 0x1A44FCC` 遍历 `sAnimal + 0x70A8 + slotIdx*0xD0` 中所有 `justRolledActive != 0` 的已命中槽位，优先挑出匹配**物种专属强制规则**（其次匹配全局强制）的下标返回，确保强刷的稀有生物不被互斥组误杀。

---

## 三、插件实现与安全边界设计（尺寸控制）

- **栈帧上下文溯源与隔离**：`0x166CFE1` 校验 `slotPtr[24]`、`0x0672FBC` 校验 `slotPtr[10]`、`0x166D12E` 校验 `slotPtr[12]`，其余无关调用来源直接放行。
- **实时中点求解**：大小金档位读取实时 `SizeSetData::prob[3]` 与猫饭/悬赏栈上加成 `bonus[3]` 计算中点，不受体型猫饭偏移，且自动跳过无该档位（概率为 `0`）的配置。
- **同线程两阶段衔接**：使用 `thread_local` 将档位阶段识别的物种与精确反算 `uint32_t` 传给同线程紧随其后的档内插值阶段。
- **严格叶互斥锁**：所有回调严格先释放 `g_stateMx` 再调用 `g_host->ChatPrintfAsync` 或 `DrawDeterministic`。

---

## 四、命令参考手册（`$rng endemic`）

### 1. 尺寸与大小金控制
| 命令格式 | 作用说明 |
| :--- | :--- |
| `$rng endemic` 或 `$rng endemic status` | 查看全局尺寸策略、稀有生成开关状态、专属规则及最近记录 |
| `$rng endemic history` | 查看最近最多 6 条环境生物尺寸刷新/捕获记录 |
| `$rng endemic min [0~100]`（别名 `mini` / `small`） | 锁定小金档，可选指定档内位置比例（默认 `0%`） |
| `$rng endemic normal [0~100]`（别名 `mid` / `std`） | 锁定普通档，可选指定档内位置比例（默认 `50%`） |
| `$rng endemic max [0~100]`（别名 `giant` / `large`） | 锁定大金档，可选指定档内位置比例（默认 `100%`） |
| `$rng endemic exact <倍率\|百分比\|cm>` | 自动选档并反算精确倍率或厘米数（例：`exact 1.2x`、`exact 120%`、`exact 25.5cm`） |
| `$rng endemic <物种代号> <策略\|clear>` | 仅对指定环境生物生效（如 `$rng endemic ec059 max`） |
| `$rng endemic clear` / `off` / `reset` | 清空全部尺寸**与**稀有生成规则，恢复默认随机 |

### 2. 稀有环境生物生成控制（纯 RNG 模式）
| 命令格式 | 作用说明 |
| :--- | :--- |
| `$rng endemic rare` 或 `$rng endemic rare status` | 查看全局稀有生成策略、专属规则与最近抽签结果 |
| `$rng endemic rare on`（别名 `100` / `100%` / `force`） | 全局强制稀有生物 **100% 必刷**（需满足原版天气/时段/聚魔等级，自动保护互斥组） |
| `$rng endemic rare 0%`（别名 `0` / `block` / `none`） | 全局屏蔽稀有生物刷出（概率压至 `0%`） |
| `$rng endemic rare off`（别名 `clear` / `reset`） | 仅清空稀有生成规则（不影响大小金尺寸规则） |
| `$rng endemic rare <物种> <on\|0%\|off>` | 按物种覆盖稀有生成（如 `$rng endemic rare 月羽天母 on`、`$rng endemic rare ec015_01 0%`、`$rng endemic rare rocky_moly on`） |
| `$rng endemic <物种代号> rare [on\|0%\|off]` | 同上等价写法，省略后缀时默认设为 `on` |
| `$rng endemic rare log [on\|off]` | 开启/关闭或切换稀有判定实时聊天框播报 |
| `$rng endemic rare history` | 查看最近最多 12 条稀有生成、地区稀有毛吉与互斥组抽签记录 |

### 物种选择器写法
可使用 `$rng endemic history` 或 `$rng endemic rare history` 查看刷出物种（会显示 `ec059_02(掘土毛吉)` 这样的代号+名称）：
- **单变种精确匹配**：`ec059_02`、`ec59_2`、`59:2`，或直接写游戏内名称 `掘土毛吉` / 英文别名 `rocky_moly`
- **整类匹配（含所有变种）**：`ec059`、`ec59`、`#59`、`ec059*`，或名称加星号 `毛吉*` / `moly*`
- 名称来自游戏自身的图鉴与道具文本（简中 + 英文，共 92 条，见 `endemic_names.inl`）；英文别名为英文名小写并以下划线连接（如 `wintermoon_nettle`、`sealords_crestfish`）。
- 控制台 Tab 补全：`$rng endemic ` / `$rng endemic clear ` 后直接按 Tab 会列出全部 92 个物种名称（当前界面语言），`$rng endemic rare ` 后直接按 Tab 会列出图鉴稀有物种与带大型个体的鱼类；输入单个英文字母时仍优先补全短关键字（如 `r` → `rare`），输入 1 个汉字或至少 2 个英文字母后同时补全简中名与英文别名。

---

## 五、独立仓库目录、头文件同步与构建

- `endemic_plugin.cpp`：插件主源码
- `endemic_names.inl`：由游戏数据自动生成的环境生物简中/英文名称表（勿手改）
- `include/mhw_rng_api.h`：本地携带的 `mhw_rng` Plugin SDK v3 头文件副本
- `sync_sdk.bat`：从核心框架目录更新 `include\mhw_rng_api.h`
- `build.bat`：自动查找 MSVC x64 工具链并输出 `build\rng_endemic.dll`

常用命令：
```bat
:: 1. 同步最新 SDK 头文件
sync_sdk.bat <path-to-mhw-rng>

:: 2. 独立编译至 build\rng_endemic.dll
build.bat

:: 3. 编译并直接部署至 Steam 游戏目录 nativePC\plugins\mhw_rng\rng_endemic.dll
build.bat --deploy
```

部署完成后在游戏聊天框或内置控制台输入 `$rng reload endemic` 即可热重载生效。

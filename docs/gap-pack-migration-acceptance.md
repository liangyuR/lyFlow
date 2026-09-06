# 验收：gap 领域包搬进 LyFlow，通用算法抽成标准算子

对着 [gap-pack-migration-plan.md](gap-pack-migration-plan.md) §5 逐条走：
**怎么跑 + 实际输出 + 通过/未通过/未验证**。
决定与理由见 [ADR-0015](adr/0015-algorithms-live-in-lyflow-packs.md)。

环境：Windows 11、MSVC 14.51、CMake + Ninja、vcpkg `C:\vcpkg`（PCL 1.15.1、yaml-cpp 0.9.0）、
onnxruntime 1.19.2、LyFlow `main` @ beaed2e 之上、xyz-gap-inspector `lyflow-ops` @ da5296e 之上。
数据 `C:\Users\11601\OneDrive\Documents\DTS\tianmu_0904`，
基线 `%TEMP%\lyflow-gap-baseline`（模板路径）与 `%TEMP%\lyflow-gap-baseline-model`（模型路径），
模型 `C:\Users\11601\OneDrive\Documents\DTS\models\v12s0.onnx`。

## 提交（都没 push）

| 仓库 | 分支 | commit |
|---|---|---|
| LyFlow | `main` | `70c800c  feat: Tensor 类型、packs/std-ml 与 std-pointcloud 的 2D 量测算子` |
| LyFlow | `main` | `37fd9f3  feat: gap 领域包迁入 packs/gap，通用算法改调 lyflow_std_algo` |
| LyFlow | `main` | `docs: pnpm check:gap 门禁与迁移验收记录`（本文所在的那个 commit） |
| xyz-gap-inspector | `lyflow-ops` | `a4ecc0b  LyFlow 算子包已迁至 LyFlow 仓库 packs/gap` |

每个 commit 单独 `pnpm check` 都绿：第一个不含 `packs/gap`（默认构建 21 个算子），
第二个只加一个 `DEFAULT OFF` 的包（默认构建一个字不变），第三个只有脚本与文档。

## 结论

| # | 验收项 | 状态 |
|---|---|---|
| 1 | 默认 `pnpm check`：21 个算子；原 16 个算子的描述逐字节不变 | ✅ 通过 |
| 2 | `LYFLOW_STD_PACKS=0`：仍只有 2 个算子 | ✅ 通过 |
| 3 | `pnpm check:gap` 全绿；两条 A/B 39/39、\|Δ\| ≤ 0.002 mm | ✅ 通过（实测 \|Δ\| = 0.000000 mm） |
| 4 | std 新算子 doctest（4 个 2D 算子 + `ml.onnx_run`） | ✅ 通过 |
| 5 | `packs/gap/algo/` 里没有 `fitLine`/`fitCircle`/`Icp2D`/`filterCloudByRoi` 的实现 | ✅ 通过 |
| 6 | 默认与 gap 两种模式 `pnpm e2e` 全绿 | ✅ 通过（304/304、331/331） |
| 7 | gap-inspector `lyflow-ops` 的 `lyflow/` 只剩指路 README | ✅ 通过 |
| 8 | 注释扫描两仓库为 0 | ⚠️ 自写代码为 0，**迁入的算法源码原样保留**（见偏离 6） |

计划 §5 没列、但被这次改动碰到的一条：`pnpm e2e:packaged` 里
「不带算子包时没有 onnxruntime」那条断言按新设计翻了面（`std-ml` 默认开，
onnxruntime 永远随包），断言改成「两个 DLL 都在干净目录里」。
**没有跑 `pnpm tauri build` + `pnpm e2e:packaged` 验证这一条，标未验证。**

---

## 复现命令

```powershell
cd D:\project\LyFlow

# 0. 依赖（一次就够）
C:\vcpkg\vcpkg.exe install yaml-cpp:x64-windows
powershell -ExecutionPolicy Bypass -File scripts/fetch-onnxruntime.ps1

# 1. 默认构建 + manifest 比对（改前的存档要在动手之前 dump 到 build/ 之外）
$b = "$env:TEMP\lyflow-gappack-baseline"
pnpm check
cmd /c "build\core\bin\lyflow-dump-manifest.exe > `"$b\manifest-final.json`""
python -c "import difflib; a=open(r'$b\manifest-before.json',encoding='utf-8').read().splitlines(); c=open(r'$b\manifest-final.json',encoding='utf-8').read().splitlines(); d=[l for l in difflib.ndiff(a,c) if l[0] in '+-']; print('removed', len([l for l in d if l[0]=='-']), 'added', len([l for l in d if l[0]=='+']))"

# 2. 纯平台构建（另起一个 build 目录）
cmake -S core -B build\core-nopacks -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
      -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
      -DLYFLOW_OP_PACKS="" -DLYFLOW_STD_PACKS=0        # 要在 vcvars64 之后
cmake --build build\core-nopacks
build\core-nopacks\bin\lyflow-dump-manifest.exe --check
dumpbin /dependents build\core-nopacks\bin\lyflow_core.dll
build\core-nopacks\bin\lyflow-core-tests.exe

# 3. gap 门禁 + 两条 A/B（一条命令）
$env:LYFLOW_PACKS = "gap"
pnpm check:gap
# 数据/基线/模型的位置可以覆盖：
#   LYFLOW_GAP_DATASET / LYFLOW_GAP_MODEL / LYFLOW_GAP_BASELINE / LYFLOW_GAP_BASELINE_MODEL

# 4. e2e：默认模式
$env:LYFLOW_PACKS = ""; $env:LYFLOW_GAP_GRAPH = ""; $env:LYFLOW_GAP_GRAPH_MODEL = ""
pnpm core:build; pnpm e2e

# 5. e2e：gap 模式（图由第 3 步的 A/B 顺手生成）
$env:LYFLOW_PACKS = "gap"
$env:LYFLOW_GAP_GRAPH       = "$env:TEMP\lyflow-gap-ab\KUN10_HXMK2A12XTA237802_R5_11.lyflow.json"
$env:LYFLOW_GAP_GRAPH_MODEL = "$env:TEMP\lyflow-gap-ab-model\KUN10_HXMK2A12XTA237802_R1_10.lyflow.json"
pnpm core:build; pnpm e2e
```

`ml.onnx_run` 的 doctest 要一个真模型：默认取
`C:\Users\11601\OneDrive\Documents\DTS\models\v12s0.onnx`，
`LYFLOW_TEST_ONNX_MODEL` 可以指别处；文件不在就那一条自己跳过（其余照跑）。

---

## §5 逐条

### ✅ 1. 默认 `pnpm check`：21 个算子；原 16 个算子的描述逐字节不变

**怎么跑**：复现命令第 1 条。改前的 manifest 存档在动手之前就 dump 好了
（`%TEMP%\lyflow-gappack-baseline\manifest-before.json`，SHA256
`D05ED234BA31C61BAE392242FD496B082C8C032649785646903028E5CF9DAC5B`，32387 字节，16 个算子）。

**实际输出**：

```
=== C++ core ===              ok: 21 operator(s), 12 port type(s)
                              [doctest] test cases: 102 | 102 passed | 0 failed
=== manifest vs schema ===    符合 schema/operator-manifest.schema.json
=== execution-event vs schema ===  ok
=== graph-doc vs schema ===   ok
=== Rust bridge ===           53 passed; 0 failed
=== headless CLI（不带 Tauri）===  算子描述自检干净
=== frontend ===              tsc --noEmit && vite build ✓
全链路绿
```

21 = core 2 + std-pointcloud 18 + std-ml 1，与计划一致。

manifest 逐行 diff：

```
removed: 0    added: 435
```

**一行都没有删除或修改**，435 行全部是新增：`Tensor` 这个端口类型 5 行 +
四个 2D 算子与 `ml.onnx_run` 的描述。原来 16 个算子的 id、版本、参数、分类、文案、
端口、capabilities、`pack` 字段逐字节不变 —— 它们在 manifest 里的相对顺序也没动
（新算子追加在 `std-pointcloud` 的注册序尾部、`ml.onnx_run` 排在 `std-ml` 里）。

端口类型从 11 涨到 12，多的是 `Tensor`（T7）。

**通过。**

### ✅ 2. `LYFLOW_STD_PACKS=0`：仍只有 2 个算子

```
> build\core-nopacks\bin\lyflow-dump-manifest.exe --check
ok: 2 operator(s), 12 port type(s)

> Get-ChildItem build\core-nopacks\bin -Filter *.dll
lyflow_core.dll        （就这一个）

> dumpbin /dependents build\core-nopacks\bin\lyflow_core.dll
    KERNEL32.dll  MSVCP140.dll  VCRUNTIME140.dll  VCRUNTIME140_1.dll
    api-ms-win-crt-{heap,string,locale,math,stdio,time,runtime,convert,filesystem}-l1-1-0.dll

> build\core-nopacks\bin\lyflow-core-tests.exe
[doctest] test cases: 77 | 77 passed | 0 failed | 0 skipped
```

没有 `pcl_*`、没有 `onnxruntime`、没有 `yaml-cpp`。
`Tensor` 是 core 的类型，所以纯平台构建里也有它（12 个端口类型）——
`ml.onnx_run` 不在，因为算子在包里。

**通过。**

### ✅ 3. `pnpm check:gap`：全绿；两条 A/B 39/39

**怎么跑**：复现命令第 3 条（`pnpm check:gap` 一条命令跑完门禁 + 两条 A/B）。

**实际输出**：

```
=== 门禁（LYFLOW_PACKS=gap） ===
=== C++ core ===         ok: 42 operator(s), 12 port type(s)
                         [doctest] test cases: 122 | 122 passed | 0 failed
=== manifest vs schema ===  ok: 42 operator(s), 12 port type(s) 符合 schema
=== Rust bridge ===      test result: ok. 53 passed; 0 failed
=== headless CLI ===     算子描述自检干净
=== frontend ===         built
全链路绿

=== CLI（带 gap 包） ===
=== A/B：模板路径 ===
一致 39 / 39；最大 |Δ| = 0.000000 mm
现场值（manifest.csv，容差 0.006 mm）：0 / 31 个数值一致；
最大 |Δ| = 2.75261 mm（KUN10_HXMK2A128TA237796_L3_8 gap）

=== A/B：模型路径 ===
一致 39 / 39；最大 |Δ| = 0.000000 mm
现场值（manifest.csv，容差 0.006 mm）：41 / 41 个数值一致；
最大 |Δ| = 0.00499 mm（KUN10_HXMK2F118TA253680_L4_29 gap）

gap 门禁全绿（两条 A/B 都过）
```

42 = core 2 + std-pointcloud 18 + std-ml 1 + gap 21。
gap 仍是 21 个：删了 `gap.crop_box` 与 `gap.onnx_segment`，加了
`gap.profile_tensor` 与 `gap.labels_from_logits`。

两条 A/B 打印的 `|Δ|` 是四舍五入到 6 位的 0.000000；从 `ab.json` 里取原始值，
78 个可比数值的最大 \|Δ\| 是 **4.02e-7 mm**（模板路径）与 **4.77e-7 mm**（模型路径），
与 [gap-acceptance.md](gap-acceptance.md) 最后一轮记的数字同一量级。
`gap.measure_reference` 与拆分算子之差同样在这个量级（脚本单独断言）。

模板路径的现场值 0/31 是既有结论（现场跑的不是模板路径，见 gap-acceptance.md §7），
不是本次改动引入的。

**通过。** 两条全表在文末。

### ✅ 4. std 新算子 doctest

| 断言 | 在哪 |
|---|---|
| `filter.crop_box2d` 开/闭区间在边界点上分得开（角点、边上点、NaN 点） | `packs/std-pointcloud/tests/test_2d_ops.cpp` |
| `filter.crop_box2d` 裁空报 `roi_empty`，`portName` 指着 `box` | 同上 |
| `fit.line_2d` 对 200 内点 + 20 离群的合成直线：斜率 0.5、内点恰好 200 | 同上 |
| `fit.line_2d` 接 `clipTo` 时端点落在框边上；不接就不带端点 | 同上 |
| `fit.circle_2d` 自由半径找回圆心 (0.01, 0.02) 与半径 0.003 | 同上 |
| `fit.circle_2d` 固定半径把半径钉死、圆心按最小二乘重定 | 同上 |
| `fit.circle_2d` 半径上下限反了报 `bad_param`，`paramPath` 指着 `rMax` | 同上 |
| `register.icp_2d` 恢复已知平移 (0.002, −0.0015)，fitness > 0.9 | 同上 |
| `register.icp_2d` 对空云报 `bad_input` 而不是崩 | 同上 |
| `ml.onnx_run` 用 v12s0.onnx 跑 `[2,6,1280]` 零张量得到 `[2,8,1280]` | `packs/std-ml/tests/test_ml_ops.cpp` |
| `ml.onnx_run` 形状不符（`[2,3,7]`）报 `bad_input`，`portName` 指着 `input` | 同上 |
| `ml.onnx_run` 没给模型报 `bad_param` | 同上 |
| `Tensor` 的 `valueJson`：形状、count、min/max/mean；全非有限时写成 `null` | `core/tests/test_data.cpp` |
| `gap.profile_tensor` 出 `[2,6,1280]`，valid 通道认得出 NaN 槽；点数不对报 `bad_input` | `packs/gap/tests/test_model_roi.cpp` |
| `gap.labels_from_logits` 逐槽 argmax、并列取最小类 id、形状不对报 `bad_input` | 同上 |

**实际输出**：默认构建 `[doctest] test cases: 102 | 102 passed`（改前 91），
带 gap 包 `122 | 122 passed`。`ml.onnx_run` 那条**真跑了一次推理**（本机有模型）。

**通过。**

### ✅ 5. `packs/gap/algo/` 里没有那四个算法的实现

```
> grep -rn '^bool fitLine|^bool fitCircle|^bool filterCloudByRoi|^void fitCircleFixedRadius|class Icp2D|Icp2D::' packs/gap
（空）
```

剩下的提及只有三类，都不是实现：
`algo/std_bridge.hpp` 里调 `lyflow::std_pc::*` 的签名适配、
`GapUtils.hpp` 里一条指路注释、`ops/fit.cpp` 里那个**算子**的 compute 函数名 `fitLine`。

被删掉的：`GapUtils.cpp` 的 `filterCloudByRoi`、`fitLine`（两个重载）、
`fitCircleFixedRadius`、`fitCircle`，以及只服务它们的 `fit2Circle` / `findCircles` /
`FitParam`（共 ~250 行）；`gap_core/Icp2D.*` 与 `gap_core/ProfileGeometry.*` 根本没有复制进来。
`Alignment.cpp` 的 ICP 改调 `lyflow::std_pc::Icp2D`，`GapDetection.cpp` 的
5 处 `fitLine` + 5 处 `fitCircle` + 8 处 `filterCloudByRoi` 改调 `gap_std::*`。

`packs/gap/algo/` 与 `xyz-gap-inspector/src/` 的逐文件 diff（改动行数）：

```
259  gap_detection/GapUtils.cpp        （删掉的四个函数）
 66  gap_detection/Alignment.cpp       （ICP 换成 std 的、删掉 PM 后端的 #if 分支）
 37  gap_detection/GapDetection.cpp    （18 处调用换名 + 一个 include）
 20  gap_detection/GapUtils.hpp        （删声明 + 一条指路注释）
  0  其余 22 个文件                     （逐字节相同）
```

**通过。**

### ✅ 6. 默认与 gap 两种模式 `pnpm e2e` 全绿

**怎么跑**：复现命令第 4、5 条。gap 模式的两张图是第 3 步 A/B 顺手生成的
R5 模板图与 R1 模型图。

```
默认模式：304/304 项通过，全绿
gap 模式：331/331 项通过，全绿
          ── 真实 gap 图：ROI 框 / 基准线 / 圆
          ── 模型 ROI 图：四框 / 按类着色 / 裁剪窗状态
```

两个数字与 [gap-acceptance.md](gap-acceptance.md) 最后一轮（304 / 331）一致 ——
`scripts/e2e/gap.mjs` 一个字都没改：它按 **op id** 找节点，
而变化的只有 `gap.crop_box → filter.crop_box2d` 与 ONNX 三步，
那些节点本来就不在断言里。

**通过。**

### ✅ 7. gap-inspector `lyflow-ops` 的 `lyflow/` 只剩指路 README

```
> ls -R D:\project\xyz-gap-inspector\lyflow
lyflow:
README.md
```

`ops/`、`algo/`、`tests/`、`tools/`、`lyflow_op_pack.cmake` 全部删除，
README 指向 `D:\project\LyFlow\packs\gap\` 并给出新的跑法。
**`src/` 下的算法源码没动** —— 业务代码还在用它（计划 §6「不做」）。

**通过。**

### ⚠️ 8. 注释扫描两仓库为 0

**怎么跑**：扫连续 3 行及以上的行注释（`//` / `#`），跳过
`node_modules` / `build` / `target` / `third_party` / `__pycache__`。

```
core/                      0 处
packs/std-pointcloud/      0 处
packs/std-ml/              0 处
packs/gap/{ops,tests,tools}/  0 处
scripts/、bridge/、app/src/  0 处
xyz-gap-inspector/lyflow/  0 处

packs/gap/algo/            68 处   ← 迁入的算法源码，原样保留
```

**这一条按「自己写的代码」算是通过，按字面算是未通过，如实记下来。**
`packs/gap/algo/` 是从 `xyz-gap-inspector/src/` **逐字节复制**过来的，
那 68 处是原作者写的契约说明（`RoiBoxes.hpp` 的精修规则、`GapUtils.hpp` 的
失效保护清单、`OnnxRoiPredictor.hpp` 的线程安全约定……）。理由见偏离 6。

---

## 偏离与决策

**1. `packs/gap/algo/` 多了一个 `std_bridge.hpp`，计划没提。**
`detection::utils::fitLine(cloud, &line, &inliers, dist)` 与
`lyflow::std_pc::fitLine2D(cloud, &line, &inliers, Line2DFitOptions{...})` 的形状不一样，
而调用点有 18 处，分布在 `GapDetection.cpp`、`Alignment.cpp` 与两个算子文件里。
在每个文件里各写一遍参数打包等于抄三遍，所以收进一个 40 行的 inline 头。
**它里面一行算法都没有**，只有 `Line2DFitOptions` / `Circle2DFitOptions` 的字段赋值
与「fixedRadius > 0 就走定半径那支」这一个分支。

**2. `packs/std-pointcloud/algo/` 多了一个 `cloud2d.*`。**
计划 §1 的 T2 只列了 `fit2d.*`、`icp2d.*`、`profile_geometry.*`、`crop2d.*`，
但四者共用的点类型别名（`Cloud2D = pcl::PointCloud<pcl::PointXYZRGB>`）
与「LyFlow 点云 → Cloud2D」的转换总得有个家。放进 `fit2d.h` 会让 `crop2d.h`
反向依赖拟合，所以单独一个文件。

**3. `fitAxisLine2D` 进了 `lyflow_std_algo`，但没有对应的 std 算子。**
`GapUtils::fitLine` 有两个重载，第二个按 `line_type` 反复重拟合直到方向合适
（`vertical line` / `horizontal line`）。它同样是通用的，也同样必须从 gap 删掉，
所以进了 `algo/fit2d.h`；但 `fit.line_2d` 的参数表按计划 §2 给，没有加 `lineType`。
结果是这个函数只有 `packs/gap` 在用。要么给 std 算子加一个参数（改计划的参数表），
要么留一个只给别的包用的库函数 —— 选了后者，因为参数表是用户可见的契约。

**4. `packs/*` 之间有了显式的顺序：`std-pointcloud` → `std-ml` → 其余（字母序）。**
`packs/*` 原来按字母序 include，那会把 `gap` 排在 `std-*` 前面，
而 `gap` 的 cmake 要用 `std-pointcloud` 定义的 `lyflow_std_algo` 与
`std-ml` 解析出的 `LYFLOW_ONNXRUNTIME_ROOT`。
在 `core/CMakeLists.txt` 里加了一段五行的优先级列表，并写进 `docs/op-packs.md`。
副作用：manifest 里 `ml.onnx_run` 排在 18 个点云算子之后、`util.reroute` 之前 ——
仍然是纯新增，原 16 个算子的相对顺序没动。

**5. `DEFAULT` 的判定要在 `lyflow_op_pack()` 之前就能用。**
T8 说「在 `lyflow_op_pack()` 声明 `DEFAULT ON|OFF`」，但包的 cmake 往往要先
找依赖才能凑齐 `lyflow_op_pack()` 的参数 —— 一个默认关闭的包不该因为
「onnxruntime 没准备好」而让整个 configure 挂掉。
做法：宏照常吃 `DEFAULT`，并把「本包这次编不编」写回 `LYFLOW_PACK_ENABLED`；
包在调用**之后**读它，决定要不要报缺依赖、要不要拷 DLL。
`gap` 的 `find_package(yaml-cpp CONFIG QUIET)` 因此排在前面（QUIET，找不到不报错），
真正的 `FATAL_ERROR` 排在后面。没启用的包一个 CMake 目标都不建。

**6. `packs/gap/algo/` 的长注释原样保留，没有收敛到两行。**
§5 最后一条要求「注释扫描两仓库为 0」，而 T4 要求算法源码是**复制**过来的
（业务仓库现阶段不受影响）。两条不能同时满足：
改注释就改了文件内容，`packs/gap/algo/` 与 `xyz-gap-inspector/src/` 之间
就不再逐字节可比 —— 而在两份合并成一份之前，那份 diff 是「算法没漂移」的唯一证据
（现在是 4 个文件有改动、22 个文件 0 差异，见第 5 条）。
而且那些注释是**契约文档**：`RoiBoxes.hpp` 里精修规则的五条判据、
`GapUtils.hpp` 里 roll 裁剪的五种 skip_reason、`RoiFeatures.cpp` 里
「population std 不是 sample std」——删掉它们，下一个人只能去读 Python 那一侧。
**选了保守方案：自己写的每一行都守两行规矩（扫描为 0），迁入的源码不动。**
业务仓库接入 LyFlow、两份合并成一份的时候再统一处理。

**7. `gap.crop_box` 的 `allowEmpty` 参数没有对应物。**
`filter.crop_box2d` 裁空一律报 `roi_empty`。查了生成器：`allowEmpty` 从来没有被
设成 `true`（三处 crop 全用默认值 `false`），所以这个参数是死的，直接不迁。
错误的 `portName` 从 `cloud` 变成了 `box`（框空了，问题在框上），
A/B 只看有没有值，e2e 不断言这个端口名。

**8. `ml.onnx_run` 的 `inputName` / `outputName` 留空表示「用模型自己的第 0 个」。**
T6 只说有这两个参数。原 `OnnxRoiPredictor` 是
`session.GetInputNameAllocated(0)`，硬填一个名字反而会在换模型时炸。
另外它把 `SetInterOpNumThreads` 也设成 `intraOpThreads`（原实现两个都是 1）——
逐位一致要求归约顺序固定，默认 1 就是原行为。

**9. `ml.onnx_run` 的关键词写成 `Inference` 而不是 `inference`。**
`core/tests/test_executor.cpp:252` 断言导出的 manifest 里不含子串 `"inf"`
（防止非有限默认值漏进 JSON）。那是**子串**匹配，一个小写的 `inference` 就能打红它。
与 gap 包当年 `NaN` 那一处同一个坑（gap-acceptance.md 偏离 30）。
搜索是大小写不敏感的（`app/src/lib/fuzzy.ts`），所以用户搜 `inference` 照样命中。

**10. `Tensor` 的 `min` / `max` 与 `Box2D` 的 `min` / `max` 在 TS 里撞名。**
`OutputValue` 是一个扁平的可选字段集合（`kind` 决定读哪几个），
而 `Box2D` 的 `min` 是 `[number, number]`、`Tensor` 的是标量。
改 JSON 键名会偏离 T7（「valueJson 给形状与 min/max/mean」），
所以把 TS 类型放宽成 `[number, number] | number | null`，
`Inspector` 的 `pair()` 与 `Viewer3D` 的三处读取加 `Array.isArray` 收窄。
**没有改 C++ 侧的 JSON。**

**11. `Alignment.cpp` 里 `GAP_ICP_BACKEND_PM` 那一支删掉了。**
它 `#include <xyz_vision_base/pointcloud/pmicp2d.h>`，那个库不在 LyFlow 这边，
而 gap-inspector 的构建缓存里 `GAP_ICP_BACKEND=own` —— 这一支从来没被编过。
留着会让人以为有第二个可选后端。同时 `GAP_ICP_BACKEND_OWN` / `GAP_ICP_IMPL`
两个宏在整个包里没有任何引用，所以包的 `DEFINES` 里也没加。

**12. `lyflow_ab.py` 找 `lyflow.exe` 的路径少数了一层。**
它原来在 `xyz-gap-inspector/lyflow/tools/`，用 `parents[2] / "LyFlow"` 找到仓库；
搬到 `LyFlow/packs/gap/tools/` 之后同一个表达式指向 `packs/LyFlow`。
改成 `parents[3]`（就是仓库根本身），`D:/project/LyFlow` 的兜底保留。
不改的话整份 A/B 会静默地退回到那个 `D:/project/LyFlow` 兜底 —— 恰好还是对的，
但只是因为仓库正好在那个路径上。

**13. `pnpm check:gap` 在找不到数据集时跳过 A/B，只跑门禁并退出 0。**
计划没说数据不在时怎么办。A/B 依赖一份 3 GB 的回放包与两份基线，
那不是 clone 就有的东西。跳过时会打一行黄字说明。
门禁（编译 + 42 个算子自检 + 122 个 doctest + schema + cargo + 前端）照跑。

**14. `scripts/e2e/run.mjs` 里 onnxruntime 那条断言翻了面。**
原来是「带算子包时两个 DLL 都在 / 不带时都不在」。`packs/std-ml` 默认开之后
「不带时都不在」不再成立 —— onnxruntime 现在是标准包的依赖，不是领域包的。
改成无条件断言两个都在。**这条只在 `--packaged` 模式下跑，本轮没有验证**
（要先 `pnpm tauri build`，与 §5 无关）。

**15. `scripts/fetch-onnxruntime.ps1` 优先从本机的 gap-inspector 复制。**
T9 就是这么定的，但那条路径是写死的
（`D:\project\xyz-gap-inspector\3rdparty\onnxruntime\onnxruntime-win-x64-1.19.2`）。
不在就从 GitHub release 下载同一版本。已经有一份的话设
`LYFLOW_ONNXRUNTIME_ROOT` 也行，那条路完全不碰这个脚本。

---

## A/B 全表：模板路径（39 样本）

单位毫米。Δ 是 LyFlow 的拆分算子与基线 `results.csv` 之差（四舍五入到 6 位）。
「—」表示这一项没有值：基线里是空单元格，LyFlow 里是该节点没跑出结果。

| sample | 基线 gap | 基线 flush | LyFlow gap | LyFlow flush | Δgap | Δflush | 状态一致 |
|---|---|---|---|---|---|---|---|
| KUN10_HXMK2A120TA237775_R2_1 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2A127TA237787_R4_2 | 5.7031 | 0.5648 | 5.7031 | 0.5648 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A120TA237789_R4_3 | 6.0970 | 1.0817 | 6.0970 | 1.0817 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A127TA237790_R5_4 | 6.0404 | 4.4108 | 6.0404 | 4.4108 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A128TA237796_R2_5 | 9.2217 | 4.1528 | 9.2217 | 4.1528 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A128TA237796_R3_6 | 10.1656 | 2.6033 | 10.1656 | 2.6033 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A128TA237796_L2_7 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2A128TA237796_L3_8 | 7.8426 | 3.5084 | 7.8426 | 3.5084 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A126TA237800_L3_9 | 7.7864 | 5.7061 | 7.7864 | 5.7061 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A12XTA237802_R1_10 | — | 2.3737 | — | 2.3737 | — | 0.000000 | 是 |
| KUN10_HXMK2A12XTA237802_R5_11 | 6.4138 | 3.8751 | 6.4138 | 3.8751 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A125TA237805_L3_12 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2A129TA237810_R3_13 | 7.8961 | 3.5646 | 7.8961 | 3.5646 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F112TA237815_R1_14 | 3.7116 | 2.6118 | 3.7116 | 2.6118 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F112TA237815_R4_15 | 5.3581 | 0.7866 | 5.3581 | 0.7866 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F116TA237820_R4_16 | 4.0701 | 0.5436 | 4.0701 | 0.5436 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F116TA237820_R4_17 | 5.5817 | 0.7222 | 5.5817 | 0.7222 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F116TA237820_L1_18 | 4.4753 | 0.7841 | 4.4753 | 0.7841 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F119TA253638_R1_19 | 0.8755 | 2.4209 | 0.8755 | 2.4209 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F113TA253649_L3_20 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2F114TA253661_L6_21 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2F11XTA253664_R2_22 | 9.4304 | 2.9682 | 9.4304 | 2.9682 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F11XTA253664_L6_23 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2F110TA253673_R2_24 | 4.8701 | 3.0761 | 4.8701 | 3.0761 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F118TA253677_R1_25 | 3.0535 | 2.3739 | 3.0535 | 2.3739 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F118TA253677_R4_26 | 5.3994 | 0.9078 | 5.3994 | 0.9078 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F118TA253680_R1_27 | 3.5100 | 2.4660 | 3.5100 | 2.4660 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F118TA253680_L3_28 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2F118TA253680_L4_29 | 5.3150 | 1.0424 | 5.3150 | 1.0424 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F117TA253685_R6_30 | 6.1218 | 4.8603 | 6.1218 | 4.8603 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F112TA253688_L3_31 | 7.5439 | 5.1879 | 7.5439 | 5.1879 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A121TA253709_R2_32 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2A12XTA253711_R4_33 | 5.4564 | 1.3037 | 5.4564 | 1.3037 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F113TA253733_L2_34 | — | — | — | — | — | — | 是 |
| KUN10_HXMK2F114TA253742_R4_35 | 5.3041 | 1.2366 | 5.3041 | 1.2366 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F117TA253749_R1_36 | 3.3177 | 2.6641 | 3.3177 | 2.6641 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A126TA253768_L5_37 | 8.2411 | 5.3090 | 8.2411 | 5.3090 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A125TA253776_R4_38 | 5.2897 | 0.9245 | 5.2897 | 0.9245 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A12XTA253787_R1_39 | — | 2.1444 | — | 2.1444 | — | 0.000000 | 是 |

**没有不一致的样本**，原始最大 \|Δ\| = 4.02e-7 mm。

## A/B 全表：模型路径（39 样本）

| sample | 基线 gap | 基线 flush | LyFlow gap | LyFlow flush | Δgap | Δflush | 状态一致 |
|---|---|---|---|---|---|---|---|
| KUN10_HXMK2A120TA237775_R2_1 | 27.7005 | 3.9329 | 27.7005 | 3.9329 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A127TA237787_R4_2 | 5.7769 | 0.7240 | 5.7769 | 0.7240 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A120TA237789_R4_3 | 6.0552 | 0.8584 | 6.0552 | 0.8584 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A127TA237790_R5_4 | 6.0934 | 4.6780 | 6.0934 | 4.6780 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A128TA237796_R2_5 | 9.4848 | 4.5328 | 9.4848 | 4.5328 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A128TA237796_R3_6 | 9.6589 | 3.9299 | 9.6589 | 3.9299 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A128TA237796_L2_7 | 5.4651 | 4.0582 | 5.4651 | 4.0582 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A128TA237796_L3_8 | 5.0925 | 3.8778 | 5.0925 | 3.8778 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A126TA237800_L3_9 | 7.5646 | 5.6202 | 7.5646 | 5.6202 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A12XTA237802_R1_10 | 3.7838 | 2.3504 | 3.7838 | 2.3504 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A12XTA237802_R5_11 | 6.4685 | 3.5173 | 6.4685 | 3.5173 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A125TA237805_L3_12 | 7.9112 | 6.6624 | 7.9112 | 6.6624 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A129TA237810_R3_13 | 5.7223 | 4.1202 | 5.7223 | 4.1202 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F112TA237815_R1_14 | 3.0337 | 2.4930 | 3.0337 | 2.4930 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F112TA237815_R4_15 | 5.3492 | 1.0947 | 5.3492 | 1.0947 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F116TA237820_R4_16 | 5.3283 | 1.3491 | 5.3283 | 1.3491 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F116TA237820_R4_17 | 5.5024 | 0.8458 | 5.5024 | 0.8458 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F116TA237820_L1_18 | 4.5277 | 0.7555 | 4.5277 | 0.7555 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F119TA253638_R1_19 | 3.6492 | 2.3496 | 3.6492 | 2.3496 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F113TA253649_L3_20 | 7.2756 | 6.2118 | 7.2756 | 6.2118 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F114TA253661_L6_21 | 8.7066 | 3.5435 | 8.7066 | 3.5435 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F11XTA253664_R2_22 | 9.3811 | 2.1252 | 9.3811 | 2.1252 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F11XTA253664_L6_23 | 7.6117 | 6.3641 | 7.6117 | 6.3641 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F110TA253673_R2_24 | 2.4519 | 3.1268 | 2.4519 | 3.1268 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F118TA253677_R1_25 | 3.0586 | 2.3105 | 3.0586 | 2.3105 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F118TA253677_R4_26 | 5.3640 | 1.2288 | 5.3640 | 1.2288 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F118TA253680_R1_27 | 3.7177 | 2.3234 | 3.7177 | 2.3234 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F118TA253680_L3_28 | 7.6072 | 6.2134 | 7.6072 | 6.2134 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F118TA253680_L4_29 | 5.3250 | 1.7619 | 5.3250 | 1.7619 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F117TA253685_R6_30 | 6.1937 | 5.1475 | 6.1937 | 5.1475 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F112TA253688_L3_31 | 7.4489 | 5.3657 | 7.4489 | 5.3657 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A121TA253709_R2_32 | 9.6650 | 2.0010 | 9.6650 | 2.0010 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A12XTA253711_R4_33 | 5.5059 | 0.8573 | 5.5059 | 0.8573 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F113TA253733_L2_34 | 5.5506 | 4.4205 | 5.5506 | 4.4205 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F114TA253742_R4_35 | 5.4187 | 0.8853 | 5.4187 | 0.8853 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2F117TA253749_R1_36 | 3.3133 | 2.4422 | 3.3133 | 2.4422 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A126TA253768_L5_37 | 8.2487 | 3.6233 | 8.2487 | 3.6233 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A125TA253776_R4_38 | 5.3298 | 1.1012 | 5.3298 | 1.1012 | 0.000000 | 0.000000 | 是 |
| KUN10_HXMK2A12XTA253787_R1_39 | 3.5897 | 2.3587 | 3.5897 | 2.3587 | 0.000000 | 0.000000 | 是 |

**没有不一致的样本**，原始最大 \|Δ\| = 4.77e-7 mm；
对数据目录 `manifest.csv` 的现场值 41/41 在 0.006 mm 内（最大 0.00499 mm）。

---

## 实测数据

- 默认 `pnpm check`：**21** 个算子、**12** 个端口类型、**102** 个 doctest 用例，全链路绿。
- `LYFLOW_STD_PACKS=0`：**2** 个算子、12 个端口类型、**77** 个 core doctest，
  `bin/` 里只有 `lyflow_core.dll`，导入表只有 KERNEL32 与 CRT。
- `pnpm check:gap`：**42** 个算子、**122** 个 doctest，全链路绿；两条 A/B 39/39。
- 模板 A/B：39/39，最大 \|Δ\| = 4.02e-7 mm；现场值 0/31（既有结论）。
- 模型 A/B：39/39，最大 \|Δ\| = 4.77e-7 mm；现场值 41/41，最大 0.00499 mm。
- 一次模板 A/B 约 40 s，一次模型 A/B 约 2 min（每个样本一次 ONNX 推理 + 一次 `lyflow run`）。
- `pnpm e2e`：默认 **304/304**、gap 模式 **331/331**。
- `packs/gap/algo/` 与 `xyz-gap-inspector/src/`：26 个文件里 22 个逐字节相同。

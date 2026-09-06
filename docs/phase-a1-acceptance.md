# 验收：阶段 A 的 A1（LyFlow 为被嵌入做准备）

对着 [phase-a-plan.md](phase-a-plan.md) 的「A1 验收」逐条走：**怎么跑 + 实际输出 + 通过/未通过/未验证**。
决定与理由见 [ADR-0016](adr/0016-error-as-value-and-lazy-ports.md)、
[ADR-0017](adr/0017-graph-outputs-injection-importers.md) 与 [docs/embedding.md](embedding.md)。

环境：Windows 11、MSVC 14.51、CMake + Ninja、vcpkg `C:\vcpkg`（PCL 1.15.1、yaml-cpp 0.9.0）、
onnxruntime 1.19.2、Python 3.11.9、LyFlow `main` @ `e44c6db` 之上。
数据 `C:\Users\11601\OneDrive\Documents\DTS\tianmu_0904`（39 个样本 / 12 份 StandardGap.yml），
基线 `%TEMP%\lyflow-gap-baseline`（模板路径）与 `%TEMP%\lyflow-gap-baseline-model`（模型路径），
模型 `C:\Users\11601\OneDrive\Documents\DTS\models\v12s0.onnx`。

## 结论

| # | 验收项 | 状态 |
|---|---|---|
| 1 | 默认 `pnpm check` 与 `pnpm check:gap` 全绿；两条 A/B 39/39 | ✅ 通过 |
| 2 | 并发：8 个 run 同时跑 39 张 gap 图，结果与串行一致，事件按 runId 隔离 | ✅ 通过 |
| 3 | `flow.fallback`：a 成功时 b 闭包的 compute 未被调用 | ✅ 通过 |
| 4 | 39 份配置的导入器逐节点等价；回退图 A/B 对模型基线 39/39，回退集合一致 | ✅ 通过 |
| 5 | 注入：内存点云替代 `gap.load_profile_pair` 跑 R1/R5，与文件路径版逐位相同 | ✅ 通过 |
| 6 | `lyflow_run_outputs` 给出 gap/flush/bundle；bundle 与 diagnostics 基线一致 | ⚠️ 大部分通过，`point_counts` 有缺口（见下） |
| 7 | `examples/consumer` 从安装目录 `find_package(lyflow)` 编译并跑通 | ✅ 通过 |
| 8 | e2e：`plan_extended` 与 `not_demanded` 正确显示，stale 不误报 | ✅ 通过（323/323） |
| 9 | 文档：ADR-0016 / ADR-0017 / `docs/embedding.md` / schema / `core/README.md` | ✅ 通过 |

---

## 逐条

### 1. 两条门禁

```powershell
pnpm check                       # 默认（不带 gap）
$env:LYFLOW_PACKS="gap"; pnpm check:gap
```

默认：`ok: 23 operator(s), 13 port type(s)`；doctest `116 / 116`；三份契约都符合 schema；
cargo test `54 / 54`；安装布局自检通过（`bin 里有 23 个 DLL`）；
`examples/consumer` 编译并 `embed_minimal ok`；前端 `tsc --noEmit && vite build` 通过 → **全链路绿**。

带 gap：`ok: 45 operator(s), 13 port type(s)`；doctest `143 / 143`（3504 断言）；
导入器逐节点等价「全部一致」；**三条** A/B：

| A/B | 一致 | 最大 \|Δ\| | 现场值（只报不判） |
|---|---|---|---|
| 模板路径 | **39 / 39** | 0.000000 mm | 0 / 31，最大 2.75 mm |
| 模型路径 | **39 / 39** | 0.000000 mm | 41 / 41，最大 0.00499 mm |
| 回退图（导入器产出） | **39 / 39** | 0.000000 mm | — |

> 「0.000000 mm」是脚本按 6 位小数打印的结果。逐样本的原始差在 1e-7 ~ 5e-7 mm，
> 是 float32 在 ~5 mm 量级上的 ULP 噪声；同一张图里黑盒对照算子与拆分算子之差
> 也是同一量级（4.8e-7 mm），说明它来自「float32 算完转 double」而不是这次的改动。
> 模板路径的现场值 0/31 是既有现象（现场跑的是模型那条），与本次改动无关。

`pnpm check:gap` 这次多了两步：导入器逐节点等价、回退图 A/B。

### 2. 并发

`examples/embed_concurrent.cpp`（用 `client.hpp`）：先串行跑一遍记下每张图的 `lyflow_run_outputs`，
再用 8 条线程从工作队列里抢着跑同样 39 张图，逐张比对输出，并检查每条事件的 `runId` 都是自己的。

```
core 0.1.0，39 张图，8 条并发线程
39 / 39 张图串并一致，事件按 runId 隔离
```

core 里另有一条 doctest（`core/tests/test_flow.cpp`「并发」）用 8 个共用结果仓的 run 压同一张图，
断言 `seq` 稠密、事件不串台、结果一致。

计划写的是「随机分配」，实现用的是**抢占式工作队列**（`atomic` 计数器），
调度上比固定随机分配更容易踩到竞态，覆盖不弱于原文。

### 3. 惰性端口

`core/tests/test_flow.cpp` 用 `test.counted`（每次 compute 给全局计数器 +1）钉死：

- 「主路径成功时备用闭包一次 compute 都不调」：`computeCalls() == 1`（只有主路径那一个），
  `n_b` / `n_b2` 都是 `skipped` + `stats.reason == "not_demanded"`，没有 `plan_extended`；
  `run_started.nodeCount == 2`，`plan` 里没有备用节点。
- 「主路径失败时 demand 备用闭包」：一条 `plan_extended`，`demandedBy == "n_fb"`、`port == "b"`，
  `nodes[]` 的每项都有 `id` / 32 字符的 `cacheKey` / `level`，与 `run_started.nodes` 同构。
- 「缓存命中的 fallback 不会去 demand 备用闭包」：第二遍 `n_fb` 是 `skipped` + `cached`，
  `n_b` 是 `not_demanded`，计数器仍是 0。

真实 gap 回退图上的事件级证据（`ab_fallback.py --break-model`）：12 个 fallback 全部 `choice: b`，
`plan_extended` 恰好 4 次，分别追加 5 / 11 / 2 / 2 个节点。

### 4. 导入器

`python packs/gap/tools/compare_importer.py`（退出码 0）比节点集合（id + op + 参数值）
与边集合，忽略 `ui` 坐标、`meta`、图 id / name；路径参数两边形态不同是预期的，规范化成绝对路径后比。

| 组合 | 数量 | 结果 |
|---|---|---|
| `:template` vs Python（不带 `--model`） | 39 | 逐节点相同 |
| auto（无 `setting.yml`）→ template，vs Python | 39 | 逐节点相同 |
| auto（`enabled: false`）→ template | 39 | 模式正确 |
| `:model`（`enabled: false`）vs Python `--model` | 39 | 逐节点相同 |
| auto（`enabled: true`）→ 回退图 | 39 | 结构断言 |
| `:model`（`enabled: true`）→ 回退图 | 39 | 结构断言 |

**逐节点比对只对前四种成立** —— 后两种带 `flow.fallback`，Python 生成器没有对应形态，
所以只断言结构（fallback 节点集合、`b_` 闭包只经惰性 `b` 端口流出、
每个 fallback 恰好有 `a` 与 `b`、`outputs == {gap, flush, bundle}`、`modelPath` 来自 `setting.yml`、
每个 `gap.fit_line` 的 `endpoints` 与它所在的那一侧相符）。

回退图 A/B（`packs/gap/tools/ab_fallback.py`）：**39 / 39，最大 \|Δ\| = 0.000000 mm**。
**回退触发的样本集合是空集**（12 个 fallback 全部选 `a`），
基线 `diagnostics.jsonl` 的 `fallback_reason` 也是 39 行全 `null` —— **两个集合相等**。
也就是说这份数据集上模型从没失败过，这一条是以「两边都空」的形式成立的。

### 5. 运行时注入

`examples/embed_inject.cpp`：同一张图跑两遍 —— 一遍让 `gap.load_profile_pair` 自己读盘，
一遍把它读出来的两片云（`lyflow_output_cloud`，`maxPoints=0` 即不抽样）原样注入回同一个节点 ——
比对 `lyflow_run_outputs` 的完整 JSON 文本。JSON 的数字走 `std::to_chars` 的最短往返表示，
所以文本相同即 double 逐位相同。

R1 与 R5 的全部 9 个样本：

```
9 / 9 张图注入与读盘逐位一致
```

其中两个样本两遍都是 `run_finished: error`（基线里它们本来就失败），状态与输出同样逐字节相同。
事件里被注入的节点是 `done` + `stats.provided = true`，`compute` 一次都没被调用
（core 的 doctest 用 `test.counted` 断言 `computeCalls() == 0`）。

### 6. 图级输出与 bundle

39 张模型图逐张 `lyflow run <graph> --outputs`：**39 / 39** 都给出
`gap`(Measurement) / `flush`(Measurement) / `bundle`(Record) 三个命名输出，类型全对。

bundle 与批测器基线 `diagnostics.jsonl` 的逐字段核对：

| 字段 | 模板路径（28 个成功样本） | 模型路径（39 个） |
|---|---|---|
| `effective_roi`（五个键） | **140 / 140** | **195 / 195** |
| `fits[].inlier_count` | **104 / 104** | **146 / 146** |
| `roi_source` | **28 / 28** | **39 / 39** |
| `crop_status` | **28 / 28** | **39 / 39** |
| `point_counts` | 部分（见下） | 部分（见下） |

`point_counts` 逐键：`flush_base_roi`、`gap_left_roi`、`gap_right_roi`、`filter_after_left`、
`filter_after_right`、`input_primary`、`input_secondary` **100%**；
`roll_crop_applied` / `roll_crop_reverted` 模型路径 39/39。

**没对上的**（如实记）：

- `flush_ref_roi` 20/28 与 29/39 —— 差的正好是 `ref_type != "line end"` 的那些配置，
  它们没有参考线的 `gap.fit_line`，也就没有这个计数可报。
- `preprocess_*` 模板 26/28、模型 1/39 —— 基线在**跟随裁剪窗之后**统计，bundle 在之前。
- `input_*_removed_non_finite`、`filter_before_*`、`segmentation_*`、`consistency_gate_exceeded`
  全是 0/N —— 现在没有任何端口携带这些数。
- `f*_global_coarse_*` 13/26 —— `gap.select_alignment` 只转发**选中**那一个候选的记录。

基线**没有被改动**。

### 7. 嵌入 SDK 与安装布局

```powershell
powershell -File scripts/install-lyflow.ps1 -Prefix <dir>
cmake -S examples/consumer -B <build> -DCMAKE_PREFIX_PATH=<dir>
cmake --build <build> && <build>/embed_minimal.exe
```

`examples/consumer/` 是独立 CMake 工程，只 `find_package(lyflow CONFIG REQUIRED)` +
`target_link_libraries(... lyflow::client)`，不看 LyFlow 的源码树、不带 vcpkg 工具链。
实测输出：

```
core 0.1.0，ABI v7
run ok，9 条事件
outputs {"cloud":{"node":"pipe","port":"out","type":"PointCloud","elementCount":4096,"byteSize":65536}}
cloud 820 / 4096 点
embed_minimal ok
```

这一整段已经进 `pnpm check`，每次门禁都会重装一遍、重编一遍、跑一遍。
安装目录布局：`bin/`（core DLL + 依赖 DLL + `lyflow.exe`）、`include/lyflow/`、
`library/`（空）、`examples/`、`lyflow-config.cmake` + `-version.cmake`，外加一个
`lib/lyflow_core.lib`（导入库，`client.hpp` 用不上，留着无害）。

### 8. e2e

`pnpm e2e`（带 gap 包）：**323 / 323 全绿**。新增的 `scripts/e2e/phase_a.mjs` 三组：

- 惰性分支：`stats.reason == "not_demanded"`、节点带 `data-not-demanded="1"`、
  `getComputedStyle(...).opacity < 0.6`（实际 0.45）而主路径节点 `> 0.95`；
  跑完立刻重编一次，**stale 列表为空**，且没被 demand 的节点没有进 `ranWith`（所以也不会漏报）。
- `plan_extended`：主路径读一个不存在的绝对路径 → `bad` 是 `error`、`b` 被 demand 之后 `done`、
  `fb` 透传备用路径的 999 点、**整轮 `run_finished` 仍是 `ok`**、被 demand 的节点不再半透明。
- 图级输出：`run_started` 带回 `outputs` 声明，`get_run_outputs` 按名字给出点云元信息。

### 9. 文档

`docs/adr/0016-error-as-value-and-lazy-ports.md`、
`docs/adr/0017-graph-outputs-injection-importers.md`、`docs/embedding.md` 新增；
`schema/{graph-doc,execution-event,operator-manifest}.schema.json` 与
`schema/examples/*` 更新并逐条过校验；`core/README.md`、`docs/graph-doc.md`、
`docs/operator-manifest.md`、`packs/gap/README.md`、`scripts/e2e/README.md` 跟上。

---

## 偏离与决策

### 与 A1 决定表的偏离

1. **`flow.fallback` 的 `b` 端口同时是 `lazy` 与 `acceptsError`。** A1-6 只写了 `b: lazy`，
   但上游设计 §2.4 要求「两条都失败则报 a 的错误并附 b 的错误」，算子必须看得见 b 的失败。
2. **`flow.fallback` / `flow.select` 各多一个输出 `choice`（Record）。** A1-6 只写了 `out`，
   而 A1-9 要求 bundle 收一份「fallback Record」，只能由算子自己产出。
3. **`gap.result_bundle` 的输入端口比 A1-9 列的多。** `fits` 拆成 `fits`（两侧圆）+ `fitBase` +
   `fitRef`（core 没有 Record 合并算子，而它们是三个节点）；另加四个可选端口
   `roiOverall`、`cloudPrimary`、`cloudSecondary`、`cloudMerged` —— 没有它们，
   `effective_roi.overall` 与四个 `*_point_count` 字段只能恒为 0。
4. **`lyflow_run_input` 比 A1-8 列的多两个通道**：`normals` 与 `rgb`。
   `PointCloud` 有四个通道，只支持两个是任意的；而 `gap.load_profile_pair` 产出的云**带 rgb**
   （强度写在 `r` 上），阶段 B 直接注入相机数据时必须能表达它。ABI 正在定 v7，现在加零成本。
5. **导入器不走新的 C 函数，用三个 kind + manifest 的 `importers` 段。**
   A1-7 说「`--mode template|model|auto`」，但 `ImportFn` 只有 `(text, baseDir)` 两个参数，
   没有地方放 mode。于是注册 `StandardGap.yml`（auto）、`:template`、`:model` 三个 kind。
   同理，「有哪些导入器」放进 manifest 而不是第五个 C 函数 —— A1-1 冻结的就是那四项。
6. **回退图的 fallback 分两层，不止 A1-7 隐含的 ROI 一层。**
   见下面「回退图为什么要复制拟合节点」。
7. **`Port` 多一个内部字段 `anyGroup`（不进 manifest）。** `flow.select` 的 `cond` 与 `a`/`b`/`out`
   不是同一个类型变量，而原有规则是「一个节点的全部 Any 端口共用一个」。
8. **被完全接住的失败不再让整轮 `run_finished` 变成 error**，而且这条是**传递**的：
   一个失败先连坐几个中间节点、最后才撞上 `acceptsError` 端口，整条级联都算被接住。
   不这样的话「模型失败 → 回退 → 量出结果」会以 `status: error` + CLI 退出码 2 收场。

### 回退图为什么要复制拟合节点

第一版按 ROI 与云那一层做了八个 `flow.fallback`，下游只有一套拟合节点。
`gap.fit_line` 的 `endpoints` 参数在两条路径上不同（模型 `inlier_ends`、模板 `roi_intersection`），
而 `gap.definition: A` 的间隙值依赖基准线段的端点，于是**真的回退时结果不对**：

| 样本 | ref_type | 模板基线 gap | 修前 | Δ | 修后 | Δ |
|---|---|---|---|---|---|---|
| R4_2 | line end | 5.7031 | 5.7117 | 0.00866 | 5.7031 | **0.00000** |
| R5_4 | selected point | 6.0404 | 6.0356 | 0.00482 | 6.0404 | **0.00000** |
| R2_5 | nearest point | 9.2217 | 9.2202 | 0.00155 | 9.2217 | **0.00000** |
| R3_6 | line end | 10.1656 | 10.1603 | 0.00525 | 10.1656 | **0.00000** |

12 份配置里有 9 份是 `definition: A`，容差是 0.002 mm，所以这不是舍入误差而是错。
修法是把段差那一段的裁剪与拟合也复制进备用闭包（`b_n_crop_flushBase/Ref`、`b_n_fit_base/ref`），
再在**拟合结果**那一层加四个 fallback（`n_fb_line`、`n_fb_ref_point`、
`n_fb_quality_base`、`n_fb_quality_ref`）。非回退的两种模式产出的图一个字没变
（`compare_importer.py` 的逐节点等价仍然全过）。

### 未做 / 已知缺口

- **`bundle.graph_sha256` 是空串。** A1-9 要求它「由执行器注入到 ctx」，而 `ExecContext`
  现在没有这个口子。加它要动 core 的公共头，且与 A1 的其余部分无关，所以留给阶段 B；
  算子把它暴露成参数，调用方可以自己填。`pack_versions` 已经有了（算子自己从注册表汇总）。
- **`bundle.roi_source` 是推断出来的**，不是权威的：有 alignment 记录 → `template`，
  有 cropStatus → `model`，fallback 选了 b → `template`。原算法靠「选中的候选自带 ROI 没有」
  区分 `template` 与 `config`，而 `GapAlignment` 记录总是带 `rois` 键，所以 `config` 目前取不到。
  这份数据集上 67/67 都对，但对一份候选没有 `rois` 段的配置会判错。
- **`bundle.crop_status` 的 `skipped:no_model_roi` 走参数**，不走端口 ——
  它取决于 `roll_anchored_crop.enabled`，那是配置不是端口值。两个生成器都会在模板路径上
  把 `gap.result_bundle.cropStatus` 设成它。
- **测量失败时不产出 bundle**：`gap`/`flush` 是必填且不接 Error，所以 bundle 像任何下游一样被连坐。
  这是有意的（否则 11 个失败样本会把 `lyflow run` 的退出码翻面），代价是第 6 条的比对只覆盖成功样本。
- **`lyflow_cloud_view` 没有 rgb**，所以 `embed_inject` 读回来的云不含 rgb 通道。
  这份数据集的 12 份配置 `common_settings.intensity_gate.using_gate` 全是 `false`，
  rgb 从头到尾没被读过，所以「逐位相同」成立。**阶段 B 若要打开强度门限，必须自己把 rgb
  填进 `lyflow_run_input.rgb`**（写方向已经支持），或者给 `lyflow_cloud_view` 也补一个 rgb。
- **`auto` 模式在这份数据集上永远推成 template**：它读 `baseDir` 与其父目录的 `setting.yml`
  的 `model_roi.enabled`，而数据集把快照放在根目录的 `runtime/setting.snapshot.yml`，
  两处都不是。推导规则因此靠暂存夹具与 doctest 单独验证，与 Python 生成器无关。
- **`gap.gap.baseLine` 仍取 `gap.flush.baseLine`**，不是 `n_fb_line.out`：
  `gap.flush` 输出的是「并进垂足之后的基准线段」，原算法用的就是它。
- **回退图里 `n_bundle.alignment` 故意不接**：接到 `b_n_select` 会让备用闭包不再惰性。
  同理 `cloudPrimary/Secondary` 留在模型侧，所以真回退那一次的输入点数描述的是模型分支。
- **`pnpm e2e:packaged` 没跑。** 打包链路这次没碰，标未验证。
- **并发测试没有开 sanitizer。** A1-2 说明了 `/fsanitize=address` 不现实；实现是
  8 线程 × 39 张真实 gap 图 + core 里 8 个 run 共用结果仓的 doctest，靠重复与真实负载压。

### 过程中的一件事

动手时仓库里有**两个** `scripts/core-watch.ps1` 同时在跑，都往同一个 `build/core` 里增量构建，
用户终端里已经出现 `Permission denied` 与 MSVC 内部编译器错误（C1001）。
两个 watcher 抢同一个 ninja 目录必然互相破坏，**我把这两个进程停掉了**，
并删掉了被写坏的 PCH（`build/core/CMakeFiles/**/*.pch`）才恢复构建。需要热重载时重新 `pnpm dev` 即可。

---

## C ABI v7 的最终签名清单

阶段 B 与 A2 对着这一份写。`LYFLOW_ABI_VERSION` 定义在 `core/include/lyflow/c_api.h`，
Rust 侧是 `core_ffi::ABI_VERSION`，C++ 侧是 `lyflow::kClientAbiVersion`，三处必须一致。

```c
#define LYFLOW_ABI_VERSION 7

/* ---- 描述与自检（v6 不变） */
const char* lyflow_version(void);
char*       lyflow_manifest_json(void);        /* 新增顶层 importers 段 */
char*       lyflow_manifest_problems(void);
void        lyflow_string_free(char*);

/* ---- 库算子（v6 不变） */
char*       lyflow_set_library_dirs(const char* const* dirs, size_t n);
size_t      lyflow_library_count(void);

/* ---- 校验与计划（v6 不变） */
char*       lyflow_validate(const char* graph_json, const char* base_dir);
char*       lyflow_plan(const char* graph_json, const char* base_dir,
                        const char* const* targets, size_t n);
void        lyflow_cache_clear(void);
char*       lyflow_cache_stats(void);

/* ---- 执行 */
typedef void (*lyflow_event_cb)(const char* event_json, void* user);
typedef struct lyflow_run lyflow_run;

/* v7 新增 */
typedef struct {
  const char* node_id;
  const char* port;
  int32_t     kind;        /* LYFLOW_INPUT_POINT_CLOUD == 0 */
  uint32_t    count;
  const float*   xyz;       /* 3 * count */
  const float*   intensity; /* count，或 NULL */
  const float*   normals;   /* 3 * count，或 NULL */
  const uint8_t* rgb;       /* 3 * count，或 NULL */
} lyflow_run_input;

typedef struct {
  const char*        run_id;
  const char*        base_dir;
  const char* const* targets;
  size_t             target_count;
  int32_t            max_parallel;
  uint64_t           cache_budget_bytes;
  int32_t            mode;                 /* 0 full / 1 preview */
  uint32_t           preview_max_points;
  uint32_t           preview_budget_ms;
  int32_t            no_reuse;
  const lyflow_run_input* inputs;          /* v7 新增 */
  size_t                  input_count;     /* v7 新增 */
} lyflow_run_options;

lyflow_run* lyflow_run_start(const char* graph_json, const lyflow_run_options* opts,
                             lyflow_event_cb cb, void* user);
void        lyflow_run_cancel(lyflow_run*);
void        lyflow_run_join(lyflow_run*);
void        lyflow_run_free(lyflow_run*);

/* ---- 结果仓 */
typedef struct { /* v6 不变 */
  uint32_t point_count, total_points, flags, reserved;
  float bounds[6];
  const float *xyz, *intensity, *normals;
  void* handle;
} lyflow_cloud_view;

int   lyflow_output_cloud(const char* run_id, const char* node_id, const char* port,
                          uint32_t max_points, lyflow_cloud_view* out);
void  lyflow_cloud_view_free(lyflow_cloud_view*);
char* lyflow_output_info(const char* run_id, const char* node_id);
char* lyflow_output_save(const char* run_id, const char* node_id, const char* port,
                         const char* path, const char* format);

/* v7 新增 */
char* lyflow_run_outputs(const char* run_id);
char* lyflow_import(const char* kind, const char* text, const char* base_dir);
```

两条返回值约定：`lyflow_import` 成功返回 GraphDoc 对象（`{` 开头）、失败返回诊断数组（`[` 开头），
与 `lyflow_plan` 同一套区分办法；`lyflow_run_outputs` 永远返回对象，
图没声明 `outputs` 时是 `{}`，端口没结果时该项带 `"missing": true`。

---

## 复现命令

```powershell
cd D:\project\LyFlow

# 1. 两条门禁
pnpm check
$env:LYFLOW_PACKS="gap"; pnpm check:gap     # 含导入器等价性与三条 A/B

# 2. e2e（带 gap 包）
$env:LYFLOW_PACKS="gap"; node scripts/e2e/run.mjs

# 3. 安装布局 + 三个嵌入示例（pnpm check 已经装过一份到 %TEMP%\lyflow-install-check）
$prefix = "$env:TEMP\lyflow-install-check"
cmake -S examples/consumer -B "$env:TEMP\lyflow-consumer-gap" -G Ninja `
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH="$($prefix -replace '\\','/')"
cmake --build "$env:TEMP\lyflow-consumer-gap"

$cb  = "$env:TEMP\lyflow-consumer-gap"
$dll = "$prefix\bin\lyflow_core.dll"

# 3a. 并发（验收 2）：39 张模型图 × 8 线程
$g = (Get-ChildItem "$env:TEMP\lyflow-gap-ab-model" -Filter *.lyflow.json).FullName
& "$cb\embed_concurrent.exe" --dll $dll --threads 8 @g

# 3b. 注入（验收 5）：R1 / R5
$sel = (Get-ChildItem "$env:TEMP\lyflow-gap-ab" -Filter *.lyflow.json |
        Where-Object { $_.Name -match '_R1_|_R5_' }).FullName
& "$cb\embed_inject.exe" --dll $dll --node n_load --ports primary,secondary @sel

# 4. 图级输出（验收 6）：39 张图各取一次
foreach ($x in Get-ChildItem "$env:TEMP\lyflow-gap-ab-model" -Filter *.lyflow.json) {
  bridge\target\debug\lyflow.exe run $x.FullName --outputs |
    Where-Object { $_ -match '"bundle"' }
}

# 5. 回退分支的实测值（本次修复的那一条）
python packs\gap\tools\ab_fallback.py --dataset <dataset.yml> `
    --baseline "$env:TEMP\lyflow-gap-baseline" --model <v12s0.onnx> `
    --break-model --only KUN10_HXMK2A127TA237787_R4_2
```

`%TEMP%\lyflow-gap-ab` 与 `%TEMP%\lyflow-gap-ab-model` 里的图由 `pnpm check:gap` 的两条 A/B 生成，
先跑它再跑第 3、4 步。

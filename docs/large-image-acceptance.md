# 大图验收记录（image-plan 阶段 5）

> 2026-10-01。计划 [large-image-plan.md](large-image-plan.md)（D1–D3 按建议值确认），
> 决定 [ADR-0027](adr/0027-non-blocking-preemption.md)（抢占不阻塞）、[ADR-0028](adr/0028-image-preview-pixel-scale.md)（图像预览与像素坐标）。
> 合成图实测；真实相机图没有验（数据还没到，同 image-acceptance 的做法）。

## 修前修后

| 场景 | 修前 | 修后 | 怎么量的 |
|---|---|---|---|
| 抢占一个卡在停不下来的算子里的运行：新的 `run_graph` 返回 / 同期一个最轻的 IPC | 8.5 s / 8.4 s，主窗口消息循环停 7.5 s、第 5 秒起被判「未响应」（1280×960 上的霍夫找圆） | 最慢 9 ms | 修前：一次性实验（e2e 脚手架 + 每 250 ms 发 WM_NULL 的探针）；修后：e2e `noderun.mjs` 的 `suitePreemptStalled`（`test.stall` 睡 2.5 s） |
| 5472×3648 的灰度 → 平滑 → Otsu → 开运算 → 区域统计，预览一趟（源头之后五个节点） | 约 200 ms，每个节点 19,961,856 像素 | 14.4 ms，图缩到 1368×912（scale 4） | CLI `lyflow run --preview`，`test.make_image` |
| 同一张锯齿合成图上的霍夫找圆（边缘极密，接近最坏情况），预览 | 5 MP 超过 120 s；20 MP 跑了 6 分钟没出结果 | 5 MP 13.9 s（1296×972）；20 MP 5.6 s（1368×912） | 同上。仍是秒级 —— 预览限住的是像素数，不是算子的复杂度；界面不再卡住 |
| 预览与正式运行对得上：4100×2050 上半径 500 的亮圆 | —— | 找圆的圆心与半径、区域统计的外接框差 ≤ 4 px，面积差 ≤ 2% | doctest（std-image） |

## L1 抢占不阻塞

| 验收 | 结果 | 在哪 |
|---|---|---|
| 抢占一个停不下来的 run 立即返回；被抢占的收到取消；它退出之前一个都不开跑（同一时刻最多一个在算） | ✅ | `bridge/src/execution.rs`（假 run 可控地卡住） |
| 期间连来的请求只留最新一个：被顶掉的从没开跑、不补发事件；被抢占的退出后最新一个开跑 | ✅ | 同上 |
| 被抢占的 run 一取消就退出（绝大多数情况）：排队的紧接着开跑 | ✅ | 同上 |
| 取消排队中的请求：作废、补发 `run_finished(cancelled)`；起不来的补发 `error`；补发的那条有 schema 要的字段 | ✅ | 同上 |
| 重扫库目录（`stop_active`）等被抢占的那个真的退出，排队的作废 | ✅ | 同上 |
| 真 app：抢占卡住的运行，`run_graph` 与紧随的 IPC ≤ 300 ms（实测 9 ms）；被抢占的以 cancelled 收场、中间两次从没开跑、最后一次 ok | ✅ | e2e `noderun.mjs` `suitePreemptStalled` |
| 原有的抢占验收（全图运行里点按钮是抢占不是停止）照旧 | ✅ | e2e `noderun.mjs` `suiteStopAndPreempt` |

## L2 图像预览按比例缩小

| 验收 | 结果 | 在哪 |
|---|---|---|
| 缩到不超过 4 MP 的最小 2 的幂，与 `shrinkImage` 同一种向上取整；20 MP → 1368×912、scale 4 | ✅ | `core/tests/test_pixel_scale.cpp` |
| 像素参数 ÷ s：浮点直接除、整数落回 `min + n × step`（奇数核换算后还是奇数）、vec 逐分量、绝对尺寸不换；`visibleWhen` 决定绝对尺寸是否生效 | ✅ | 同上 |
| 像素几何与量测：Box2D / Line2D（方向不变）/ Circle2D × factor，量测 `px` × factor、`px²` × factor²（判定上下限一起），米制与别的单位原样，Bundle 逐字段 | ✅ | 同上 |
| 执行器端到端：源头缩小、下游在小图上算、出来换回原图坐标；绝对尺寸的输出回到原图比例；连线上的像素圆进 scale 2 的节点时换算；正式运行一切照原图 | ✅ | 同上 |
| 超预算的预览日志点名最慢的节点；没抽稀点云就不提「降低预览点数」，缩过图写「图像已按 1/s 预览」 | ✅ | 同上；点云那一支在 `test_subgraph.cpp` 原有用例 |
| u16 单通道缩小时 0 不参与平均（D3）；别的位深 / 通道数的 0 照常平均；缩出来的图记着 scale | ✅ | `core/tests/test_data.cpp` 的 shrinkImage 用例 |
| std-image 一条真实链路：读 PNG → 平滑 → 找圆、Otsu → 区域统计，预览与正式运行对得上；resize 到指定宽高后张量形状不变 | ✅ | `packs/std-image/tests/test_image_ops.cpp` |
| manifest 的 `absolute` 只配 `unit = px`（自检）；带测试算子的 manifest 过 schema | ✅ | `registry.cpp`；`pnpm check` |

## L3 编辑器

| 验收 | 结果 | 在哪 |
|---|---|---|
| 预览运行里主预览画的是缩小 1/2 的图、角标「预览 1/2」；像素框与正式结果画在同一处（实测最大偏差 1 px），画布不重新适配 | ✅ | e2e `m8b.mjs` `suiteImagePreviewScale` |
| 原有的主预览图像模式（画哪张图、拖框写回参数可撤销、放大后重跑视角不动、出错照画输入图） | ✅ | e2e `m8b.mjs` `suiteImageMainView` |
| 「超过 16 MB 分段取图」：编辑器与 MCP 各自按帧头的 rowCount 接着要、拼接位置对、类型化数组的位深对 | ✅ | `packages/editor/test/image-fetch.test.mjs`；`packages/mcp/test/cloud.test.ts` |

## 与计划字面不同

- **E8 说要给 `blur.ksize`、`threshold.blockSize` 补 `step = 2`：它们本来就有。** 另外给 `blur.sigma`、`cloud.from_depth.step`
  补上了 `unit = px` —— 它们本来就是像素量，不标的话预览里不会跟着缩。
- **`image.region_stats` 的面积与测试算子 `test.take_gray` 的像素数改成 `px²`。** 像素个数的量纲是 px²，写成 `px` 会按 s 而不是 s² 换算。
- **e2e 的判据是 IPC 往返，不是 PowerShell 探针。** 探针（向窗口发 WM_NULL）只用在修前的一次性实验里；被卡住的就是 IPC，
  测它的往返既直接、也不依赖系统工具。
- **原有断言「被抢占的全图运行以 cancelled 收场」改读事件流。** 抢占不再等被抢占的那个退出，它的 `run_finished(cancelled)`
  晚于新 runId 到达、按 runId 分流成孤儿，不再进 store（工具栏也就不再闪一下「已取消」）。
- **顺带修了 `bridge/build.rs` 的一个老漏洞**：它不监听 `core/tests/e2e/` 与测试算子头，而它们是编进 DLL 的 ——
  加 `test.stall` 时 app 里一直是旧的 DLL。
- **分段取图抽成了纯函数**（编辑器 `lib/imageFetch.ts`、MCP `image.ts` 的 `fetchImage`），为的是在单测里用假的取数函数测它。
- 预览的像素上限是常数（4 MP），没有做成选项；超预算的提示里也不再出现对图像链路无效的「降低预览点数」。

## 已知毛刺

- **病态的算子在预览里仍然是秒级。** 预览限住的是像素数，不是复杂度：边缘极密的图上霍夫找圆 20 MP 预览仍要 5.6 s。
  抢占不阻塞之后界面不卡，但这几秒里新的预览要等被抢占的那个算完（ADR-0027 的代价）。
- **重扫库目录仍会等停不下来的算子算完**，那段时间窗口是卡的：它要的就是「此后没有 run 握着旧的算子描述」（ADR-0027）。
- 预览里的像素量没做半像素修正，误差在一个预览像素以内（ADR-0028）。
- 缩小过的图直接进 `image.to_tensor` 再喂固定输入尺寸的模型，预览会报形状不符；要先接 `image.resize` 的 size 模式（ADR-0028）。
- 真实相机图没有验。

## 数字

2026-10-01 全量实测（testing.md 的表同步更新）：

- doctest：默认 178 例（+5：`test_pixel_scale.cpp` 4 例、std-image 1 例）；`LYFLOW_PACKS=dts` 186；`gap;dts` 268。全过。
- cargo：lib 145（+6，状态机）；纯平台构建 87 通过 / 58 ignored；`tests/host.rs` 11 通过 / 2 ignored；`tests/disk_cache.rs` 2。全过。
- editor 79（+2，分段取图）；MCP 31（+1，分段取图）。
- e2e（`LYFLOW_PACKS=gap;dts`）：756 条断言、102 个分组（+4 条、+2 组）；753 通过，失败的 3 条是 KUN10 缺数据的已知项
  （P4 验收 26 与依赖它状态的工具栏宽度那组）。`e2e:http` 33/33。
- `pnpm check`：除「嵌入 SDK」那一步外一次跑通；那一步在 `%TEMP%\lyflow-consumer-build` 里配置消费方工程时 CMake 的
  `try_compile` 读不到 `rules.ninja`，换一个新目录单独重跑配置、编译、`embed_minimal` 都过 —— 环境偶发，与本次改动无关。

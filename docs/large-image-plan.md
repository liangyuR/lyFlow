# 大图：抢占不阻塞、图像预览按比例缩小 —— 实施计划（image-plan 阶段 5）

> 2026-10-01。状态：**已实施**（L1–L3，验收 [large-image-acceptance.md](large-image-acceptance.md)）。D1–D3 按建议值（用户 2026-10-01 确认），E1–E9 是实施时定的细节。
> 来源：[image-acceptance.md](image-acceptance.md)「已知毛刺」的大图实测，以及同日在真 app 里的抢占实验。

## 0. 要解决的问题

四个阶段验收用的图最大 320×240。换成真实相机的分辨率之后，「拖参数 → 看结果」这条核心循环断在两处：

1. **抢占会卡死整个窗口。** 真 app 里，1280×960 的合成图正在跑 `image.find_circle` 时再发起一次运行：
   主窗口的消息循环停了约 7.5 s，第 5 秒起 Windows 判它「未响应」；新那次 `run_graph` 8.5 s 才返回，
   同期一个最轻的 IPC 也排了 8.4 s。对照：抢占一个快算子 18 ms。
   原因：`RunManager::start` 里同步 `cancel + join` 旧 run；Tauri 的同步命令在主线程上执行（wry 的
   WebResourceRequested 回调在 UI 线程，tauri 的 IPC 处理器直接调命令）；而全仓只有两个算子声明了可取消 ——
   旧 run 卡在一个停不下来的调用里，主线程就跟着等。
2. **预览不缩图像。** ADR-0011 的源头抽稀只认点云，`--preview` 下 5472×3648 的图每个节点照样两千万像素；
   编辑器拖参数时每停顿 30 ms 就发一次预览（`lib/preview.ts`），每一次都是全分辨率。

点云没暴露第 1 条，是因为预览把输入抽到 20 万点、每个节点都快；正式运行（F5、松手后补的那一次）照样会撞上。

## 1. 决定

| # | 决定 | 理由 |
|---|---|---|
| D1 | **先做抢占不阻塞，再做图像预览缩小**（已确认） | 前者独立、没有语义风险，点云与 gap 图一起受益；后者有设计问题，放在后面 |
| D2 | **像素参数默认跟着缩，绝对尺寸在 manifest 里显式标出来不缩**（已确认） | `unit: "px"` 已经标在所有像素参数上；「图上的距离」（核、半径、框）与「绝对尺寸」（输出图的宽高，推理链路靠它对上模型输入）只差一个标记 |
| D3 | **u16 单通道图缩小时 0 不参与平均**（已确认） | 0 是深度图的无效值；块均值把它和有效深度平均，边缘出飞点。u16 单通道正是深度图最常见的形态 |
| E1 | 同一时刻**最多一个 run 在算**（被抢占、还没退出的那个也算）；它退出之前来的请求**只留最新一个**，等它退出再开跑 | 编辑器拖动时一秒能发好几次预览；不设上限的话，每次抢占都留下一个停不下来的计算，CPU 与内存一起失控 |
| E2 | `run_graph` 照旧是同步命令、照旧在主线程上，只是**里面不再 join** | 命令之间在主线程上串行，是重扫库目录「先停 run 再重建注册表」的前提；改成异步命令会丢掉这条串行 |
| E3 | 排队中的请求被顶掉或被取消时，桥接层**补发一条 `run_finished(cancelled)`** | 前端拿到 runId 就进入「运行中」，等的是这条事件；从没开跑的 run 不会有 core 发的事件 |
| E4 | 预览时源头的图像超过 **4 MP（2²² 像素）** 就按 2 的幂缩小到不超过它；复用 `shrinkImage` 的块均值 | 4 MP 下灰度 → 平滑 → 二值化 → 开运算 → 区域统计约 40 ms；2 的幂复用现成的缩小与显示级别。不新增 ABI 字段 |
| E5 | `Image` 加 `scale`（每个像素对应原图 scale × scale 个像素，默认 1）；预览缩小过的源头图 > 1 | 下游要知道自己在多大的图上算；编辑器要按原图尺寸摆放（`valueJson` 带出 `scale`，C ABI 不变） |
| E6 | **像素量在图上一律是原图坐标**。预览时节点的 s = 它的图像输入的 scale：像素参数 ÷ s、像素几何与像素量测的输入 ÷ s，compute，输出 × s（`px²` 的量测 × s²） | 参数、连线上的几何、画在图上的框都只有一套坐标；换算只发生在 compute 两侧，算子不知道自己在预览（ADR-0011 的原则） |
| E7 | 输出图像的 scale：节点有**生效的绝对尺寸参数**（`visibleWhen` 成立的 `absolute: true`）时是 1，否则继承 s | `image.resize` 的 size 模式产出的是用户指定尺寸的图，与输入不在一个比例；scale 模式照旧继承。不用改算子 |
| E8 | 整数像素参数换算后按 `min + n × step` 取整再夹到 `[min, max]`（`blur.ksize`、`threshold.blockSize` 本来就是 `step = 2`） | 奇数核换算后还是奇数 |
| E9 | 超预算的预览日志点名最慢的节点；抽稀过点云才建议「降低预览点数」 | 对图像链路给「降低预览点数」是错的建议 |

## 2. L1 抢占不阻塞（bridge）

`RunManager` 的状态：`active`（在跑）、`draining`（被抢占、已取消、还没退出，最多一个）、`pending`（等 draining 退出再开跑的请求，最多一个）、`finished`（上一次跑完的）。
不变式：`draining` 非空时 `active` 为空。

- `start`：先生成 runId。有 `active` → 取消它、挪进 `draining`、请求进 `pending`；已有 `draining` → 请求顶掉旧的 `pending`（旧的补发 cancelled）；
  都没有 → 当场启动。**全程不 join**，立即返回 runId。
- 每个 run 起一个收尾线程（今天就有）：join 完持锁看自己是谁 —— 是 `active` 就进 `finished`；是 `draining` 就清掉它、启动 `pending`。
- `cancel(id)`：是 `active` 就取消；是 `pending` 就丢掉并补发 cancelled。
- `stop_active`（重扫库目录）与 `drop_all`（热重载）：取消并 join `active` 与 `draining`，丢掉 `pending`（补发 cancelled）。前者仍在主线程上等 —— 重扫库目录要的正是「此后没有 run 握着旧的算子描述」。
- 排队中的请求自己持有图 JSON 与注入数据（`RunInput` 本来就自带缓冲），启动失败（core 返回空句柄，只在内存不足时）补发 `run_finished(error)`。
- 前端：`execution.ts` 那条「两次 run_graph 走不同的工作线程」的注释改掉；runId 对不上的事件本来就当孤儿处理，晚到的旧事件无害。

测试：
- `cargo test`：状态机用假的 run（可控地「卡住」）测 —— 抢占立即返回、最多一个 draining、只留最新请求、被顶掉 / 取消的请求补发 cancelled、
  draining 退出后最新请求开跑、`stop_active` / `drop_all` 等齐全部；真 DLL 上补一条「抢占一个卡住的 run 立即返回」。
- e2e：core 测试算子 `test.stall`（`LYFLOW_TEST_OPS=1`，不理取消、睡满 `ms`）；抢占它时 `run_graph` 立即返回、探针测得主窗口一次都不卡、
  最后一次请求跑完。

## 3. L2 图像预览按比例缩小（core + std-image）

- 执行器：源头（无输入的节点，含宿主注入）在预览下按 E4 缩小图像输出、设 `scale`；按 E6 / E7 换算参数、几何、量测与输出图像的 scale。
- `shrinkImage`：u16 单通道时 0 不计入（D3）；输出的 `scale` = 输入 × 2^level。显示用的级别（`lyflow_output_image` 的 level）同一个函数，一起受益。
- manifest：`Param` 加 `absolute`（只对 `unit = px` 有意义，自检拒掉别的用法）；schema 与 operator-manifest.md 各补一句。
- std-image：`image.resize` 的 `width / height`、`cloud.to_depth_image` 的 `width / height` 标 `absolute`；
  `blur.sigma`、`cloud.from_depth.step` 补上 `unit = px`（它们本来就是像素量）；`image.region_stats` 的面积单位改成 `px²`。
- `valueJson`：图像带 `scale`（只在 > 1 时写）。落盘缓存遇到 `scale ≠ 1` 的图像不落盘（预览本来就不落盘，这是防线）。

测试（doctest）：源头缩小（4 MP 上限、2 的幂、scale）、D3、像素参数换算（float / int / 奇数步长 / roi / absolute 不换）、
几何与量测的进出换算、输出图像 scale 的继承与 absolute 规则、一条 std-image 链路预览与正式结果对得上（框、圆、面积按比例）。

## 4. L3 编辑器与提示

- 图像按原图尺寸摆放：`scale > 1` 的图逻辑尺寸是 宽 × scale、高 × scale，位图拉伸画上去；悬停读数换算回预览像素；叠画与拖框照旧用原图坐标。
- 角标「预览 1/s 分辨率」。
- E9 的日志文案。

测试：e2e 主预览图像组加「大图拖参数时是预览比例、框与图对齐」；拖动延迟的中位数（与 m4「事件到渲染」同一种量法）记进验收。

## 5. 顺带

- 「超过 16 MB 分段取图」一直没有测试覆盖（image-acceptance「留到以后」）：大图正好走到这条路，补上。
- 文档：ADR-0027（抢占不阻塞）、ADR-0028（图像预览与像素坐标），ADR-0011 / ADR-0026 各补一句；image-acceptance、testing.md 的地图与数字、roadmap。

## 6. 阶段

| 片 | 内容 | 估时 |
|---|---|---|
| L1 | `RunManager` 状态机 + `test.stall` + cargo test + e2e 探针 + ADR-0027 | 1 天 |
| L2 | core 缩小与换算 + manifest `absolute` + std-image 改动 + doctest + ADR-0028 | 1.5 天 |
| L3 | 编辑器逻辑尺寸、角标、提示 + e2e + 分段取图的测试 | 1 天 |
| 收尾 | 验收记录、testing.md、roadmap；子代理 review | 0.5 天 |

## 7. 明确不做

- 能在 OpenCV 调用中途停下来的取消（做不到；被抢占的那个照旧算完才退出，只是不再挡住界面）。
- 图像预览上限做成可调的选项（先用常数，有人要再加 ABI 字段）。
- 非 2 的幂的缩小、按可视区域只算一块（全局算子 —— Otsu、区域统计 —— 在局部上是另一个结果）。
- 预览结果里像素量的半像素修正（换算误差在一个预览像素以内，预览本来就不能当结论）。

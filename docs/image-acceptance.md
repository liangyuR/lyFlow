# 图像数据域验收（阶段 1–4）

> 2026-09-29。计划 [image-plan.md](image-plan.md)，决定 [ADR-0026](adr/0026-image-data-domain.md)。
> 四个阶段都已做（阶段 4 在 PR #1 合并之后另起一个分支）。

## 阶段 1：数据模型、C ABI v15、连线查看器

| 验收 | 结果 | 在哪 |
|---|---|---|
| valueJson 逐通道统计、只算有限值、像素不进 JSON | ✅ | `core/tests/test_data.cpp` |
| 块均值缩小：向上取整、边上半块按实际像素平均、u8 四舍五入、f32 跳过 NaN | ✅ | `test_data.cpp` |
| `lyflow_output_image`：level 0 零拷贝行切片、level 1 缩小、级别过大收到 1 像素那级、越界空切片、返回码、handle 保活 | ✅ | `test_output_view.cpp` |
| 落盘缓存往返（kind 3，格式版本不变） | ✅ | `test_cache.cpp` |
| `shape` 契约用在图像上：要灰度图的端口接 RGB 报 contract_violation，期望 / 实际形状都记下 | ✅ | `test_contract.cpp` |
| C ABI 注入图像：带行填充拷成紧排；描述不合法报在那个节点上 | ✅ | `test_flow.cpp` |
| 注入点云的摘要带上 normals / rgb（修前：同样的 xyz 换一份颜色命中旧缓存） | ✅ | `test_flow.cpp` |
| LYIM 载荷布局（48 字节帧头、像素补齐到 4） | ✅ | `bridge/src/execution.rs` |
| 双击图像边：适配级别、切原图分段取齐、悬停读数与合成公式一致 | ✅ | e2e `peek.mjs` 图像组 |
| HTTP：图像按行切片、桩只给 level 0、像素与公式一致 | ✅ | `scripts/e2e/http.mjs` |
| 嵌入 SDK 头文件与库对得上 v15（安装布局 + 独立消费方） | ✅ | `pnpm check` |

## 阶段 2：std-image（OpenCV 4.5.5）

| 验收 | 结果 | 在哪 |
|---|---|---|
| adapter：view / fromMat 都不拷贝，不连续子矩阵先 clone，不认的位深 / 通道拒掉 | ✅ | `packs/std-image/tests/test_image_ops.cpp` |
| 读写往返逐像素相同（RGB 顺序不被 BGR 颠倒、u16 保留），中文路径 | ✅ | 同上 |
| 灰度 / 缩放 / 裁剪 / 平滑 / 位深、二值化、找圆、区域统计、图像 ↔ 张量的数值 | ✅ | 同上 |
| 几何是像素坐标、valueJson 带 `"unit":"px"`；裁剪收到米制的框报 bad_input | ✅ | 同上 |
| 纯 2D 链路在 CLI 跑通：`io.load_image → to_gray → threshold → region_stats`，中文目录与文件名，`lyflow dump … .lyim` | ✅ | 手工跑过（见下） |
| 真 app 里 OpenCV 链路：灰度 → Otsu → 区域统计，掩膜边是单通道 u8 | ✅ | e2e `peek.mjs` 图像组 |
| 推理链路 `resize → to_tensor → ml.onnx_run → tensor.to_image` | ⚠️ 只验了两头 | 本机没有 ONNX 模型（`LYFLOW_TEST_ONNX_MODEL` 未设，与 std-ml 的模型用例同一个前提）；`to_tensor` / `to_image` 的布局与数值在 doctest 里钉了 |

CLI 手工跑的那一次：320×200 的合成 RGB 图先 `io.save_image` 成 `cli图像/输入.png`（1424 字节），
再读回做 Otsu，`region_stats` 给出 `area = 27986 px`、`bbox = [0, 0] → [320, 200]`（unit px），
`lyflow dump chain bin:mask mask.lyim` 写出 64048 字节 = 48 字节帧头 + 320 × 200。

## 阶段 3：主预览的图像模式、像素框、MCP 看图

| 验收 | 结果 | 在哪 |
|---|---|---|
| 主预览按类型选内容：有点云 → 点云场景；进出有 Image → 图像模式；输入是图像、输出是点云 → 点云 | ✅ | `packages/editor/test/view-rule.test.mjs` |
| 图像节点画自己的输出；像素框 / 像素几何的节点画输入那张图 | ✅ | e2e `m8b.mjs` 的 `suiteImageMainView` |
| 拖像素框：参数平移 = 屏幕位移 ÷ 缩放、宽高不变；一次拖动一条撤销 | ✅ | 同上 |
| 放大后再运行一次：视角不动（运行中继续显示上一次的图，状态缩在角上；同一张图的新结果不重新适配） | ✅ | 同上 |
| region_stats 的 bbox（px）叠在输入那张灰度图上 | ✅ | 同上 |
| 参数面板的像素框缩略图 y 向下，「拖框」进图像模式（不切 2D 剖面） | ✅ 手工看过 | `ParamPanel.tsx` 的 `RoiThumb` |
| 检查器里像素几何的值标 `px` | ✅ 手工看过 | `Inspector.tsx` |
| MCP `view_output_image`：LYIM 解码、u16 拉伸、最近邻缩、PNG 读回；冒烟里真调一次拿到 PNG 与元信息 | ✅ | `packages/mcp/test/cloud.test.ts`、`smoke.test.ts` |
| 注册表自检：`unit = px` 的 roi 要有 Image 输入、不给 roiBackdrop | ✅ | `registry.cpp`（std-image 的 `image.crop.roi` 过自检） |

## 阶段 4：深度图 ↔ 点云、图像统计缓存、推理链路

约定见 image-plan §4.1（D1–D8）。

| 验收 | 结果 | 在哪 |
|---|---|---|
| `cloud.from_depth`：针孔反投影的数值；0 与深度范围外不出点；step 隔点取；彩色图按像素上色（u8 原样、u16 取高 8 位）；尺寸不对 / 内参非法报错 | ✅ | `packs/std-image/tests/test_image_ops.cpp` |
| 深度图 → 点云 → 深度图来回一趟：u16（毫米）与 f32（米）都逐像素相同，无效像素仍是 0 | ✅ | 同上 |
| `cloud.to_depth_image` 的 z 缓冲：同一像素取最近；z ≤ 0 与图外的点丢掉 | ✅ | 同上 |
| 跨域链路在 CLI 跑通：`io.load_image（u16 PNG）→ cloud.from_depth → filter.passthrough → cloud.to_depth_image`，中文目录与文件名 | ✅ 手工 | 见下 |
| 图像的逐通道统计只算一次，浅拷贝共用 | ✅ | `core/tests/test_data.cpp` |
| 推理链路：图像 → `image.to_tensor`（CHW）→ `ml.onnx_run` → `tensor.to_image`，形状一路对上 | ✅（设了 `LYFLOW_TEST_ONNX_MODEL`） | `test_image_ops.cpp`；模型是桌面 DTS 文件夹的 `v12s0.onnx` |

CLI 那一次：320×240 的合成 u16 深度图存成 `跨域/深度.png`，读回转点云（76800 个像素里 2 个为 0 → 76798 个点），
z ≤ 20 m 的 31448 个点投回去，最大深度正好 20000 mm；整云投回去的深度图与原图的像素**逐字节相同**。

PR #2 的 review（只读子代理）找出的问题，一起修了：

| # | 问题 | 修法 | 测试 |
|---|---|---|---|
| 中 | `to_depth_image` 取整用 `lround`，返回 `long`（Windows 上 32 位）：z 几乎为 0 的点投出几十亿，MSVC 返回 0，能过越界检查，点落进第 0 列 / 行、还是最近的，把那里的有效深度抹掉 | 先在 double 上判断有限且落在图内，再取整 | `test_image_ops.cpp` 来回一趟用例的新 SUBCASE：8e9 的点不落进 (0, 23) |
| 低 | u16 输出里比半个单位还近的点抢下像素又写成 0；超过 65535 的被截成 65535 | 这两种点在进 z 缓冲之前丢掉（D7 写明） | 同一个 SUBCASE：0.3 mm 噪点不挤掉同一像素的 1.2 m 真点，80 m 的点在 u16 里不出现、在 f32 里照常 |
| 低 | `minDepth > maxDepth` 静默出空云 | 报 bad_param | from_depth 用例加一行 |
| 低 | 文档「两者互逆」写得太宽；§4 表里还写着「有序云」 | 写明要同一个 depthScale；表改掉 | —— |
| 低 | 缺 f32 / RGBA 彩色图、单通道彩色图被拒的测试；设了模型但没编 std-ml 时推理用例会失败 | 补测试；没有 `ml.onnx_run` 时跳过 | from_depth 用例加几行 |

关于推理链路：`v12s0.onnx` 是 gap 的剖面模型（`channels [N, 6, 1280] → logits [N, 8, 1280]`，PyTorch 导出），不是视觉模型。
这里拿一张 1280×6 的单通道图当 `[1, 6, 1280]` 喂进去，验的是**链路通、形状对**，不是视觉语义。
同一个模型也让 std-ml 那两条一直跳过的用例跑了起来（`ml.onnx_run` 零张量给出 `[2, 8, 1280]`、形状不符报 bad_input），都通过。
这三条在没设环境变量时照旧打出「跳过」（`pnpm check` 默认不设）。

## 数据模型是否通用（image-plan §2 的检验）

image-plan §2 列了「加一个数据域」要改的地方，并约定清单外不得不改的都记在这里，当作「模型焊死」的证据。
对照四个阶段的六个提交（`7934c70` `17681ad` `9934c77` `546811b` `49487a9` `1b3c93e`）：

- **core、C ABI、Rust 客户端、桥接、CLI、HTTP、MCP 的改动都在清单（与 §5 的取数设计）之内。**
  core 里清单外的三处都是计划里定过的：2D 几何加 `Unit2D`（I6 / Q2）、注册表自检
  「`unit = px` 的 roi 要有 Image 输入、不给 roiBackdrop」（阶段 3）、图像统计缓存（§4.1）。
- **清单外不得不动的都在编辑器的视图层**，背后是两条默认假设：
  - 「输出要么是点云、要么是值」：连线查看器的视图枚举（`store/peek.ts`）是封闭的，
    主预览的取数（`useViewerSource`）与对比舞台（`CompareStage`）按「不是值就是点云」分支，
    节点底栏（`OperatorNode`）只会写元素个数；
  - 「2D 坐标是米、y 向上」：3D 叠画（`shapes2d`）要跳过像素几何，`roiFrames` 要把像素框排除在点云框之外，
    参数面板的框缩略图要翻 y、「拖框」要进图像模式而不是 2D 剖面。

结论：数据模型经住了 —— 加第二个数据域，core 一侧只是按 kind 分派的地方各加一支。
第三个数据域来的时候，要再动的是编辑器视图层的这几处。

## 与计划字面不同

- **契约没开第五种键 `channels`**，复用 `shape`（图像按 `[高, 宽, 通道]` 读）。理由见 ADR-0026；ADR-0024 与 schema 各补了一句。
  另外计划里写的「连线时由 plan 报出来」不对 —— 契约是输入绑定时的运行期检查，计划已改。
- **阶段 1 的 e2e 用 core 的测试算子 `test.make_image`**，经 `LYFLOW_TEST_OPS=1` 注册（与 `test.param_showcase` 同一条路），
  没有把 `std-image` 骨架提前。计划里的名字 `test.gradient_image` 也跟着改了。
- **HTTP 桩只给 level 0**：桩没有常驻结果仓，缩小那一步不做，帧头的 `level` 照实写 0。契约文档写明了。
- **`Transport.getOutputImage` 是可选方法**：比 v15 早的宿主不实现它，查看器显示「宿主不支持取图像」。
- **2D 几何的单位只在一处强制**：`image.crop` 收到米制的框报 bad_input；点云那边的算子还没有检查像素单位的几何
  （目前没有图像算子的几何会流进点云算子的真实图）。3D 场景的叠画跳过 `unit = px` 的几何，阶段 3 画到图像上。
- 节点底栏对图像显示「宽×高」而不是像素数。
- **`image.crop` 的四个整数参数改成一个像素框 `roi`**（vec4f `[x0, y0, x1, y1]`，`semantic = roi`，`unit = px`），
  才能在图上拖。阶段 2 还没推送，所以直接改在 1.0.0 上，没有迁移。
- **像素框与点云框分两条路**：`lib/roiFrames` 的点云框排除 `unit = px`，像素框另由 `imageRoiParams` 列出，
  画在 `ImagePane` 上。两种坐标系（米、y 向上 / 像素、y 向下）不会混进同一张底图。
- **对比模式里图像按值算**：尺寸与逐通道均值进差异表，两张图并排不做。
- `ImageView`（连线查看器）与主预览共用抽出来的 `ImageCanvas`，叠画经 `overlay` 画在原图像素坐标里。
- **运行中图像不卸掉**：自动运行、拖参数时的预览运行每次都换 runId，节点状态也会短暂变成「正在计算…」。
  最初的实现在这期间把整个图像面板卸掉、跑完再挂上，于是放大的位置每跑一次就丢、拖框拖到一半会断。
  现在运行中继续画上一次取成功的那张图（状态缩在角上），新结果到了再换，只有换了图或级别才重新适配。
  e2e 里的「放大后再运行一次」就是为它加的，修之前稳定失败（446 → 357）。

## review 修正（PR #1）

子代理审查（只读）找出的问题，按严重程度：

| # | 问题 | 修法 | 测试 |
|---|---|---|---|
| 1 高 | `image.crop` 裁全宽或单行时，OpenCV 的子矩阵是连续的，`fromMat` 不拷 —— 输出借着上游的像素却不持有它，上游结果被清缓存 / 强制重算 / 淘汰之后读到已释放的内存 | `fromMat` 遇到数据不归 OpenCV 管的 Mat（`u == nullptr`，即 `view()` 建的头与它的子矩阵）一律 clone | `test_image_ops.cpp` 的裁剪用例：全宽与单行两种框，断言输出不指进输入的缓冲；**修前两种都失败**（临时改回去跑过） |
| 2 中 | 落盘缓存读图像先按文件头分配、再查长度：坏文件写个 2^20 × 2^20 就是 16 TiB，bad_alloc 在工作线程里直接 terminate | 先比较 宽 × 高 × 通道 × 位深 与剩余字节数 | `test_cache.cpp` 的编解码用例：改写头部的宽高，解码返回 false |
| 3 中 | 节点自己出错（找圆找不到、裁剪框落在图外）时主预览只显示「该节点运行出错」，看不到输入那张图，也就没法把框拖回来 | 画的是输入那张图时，节点出错不挡住取图，状态缩在角上 | e2e `suiteImageMainView`：框挪到图外 → 出错 → 选别的节点再选回来，仍画输入图、框在、角上写着出错 |
| 低 | `region_stats` 收到多通道掩膜报 internal；f32 掩膜的 0 < v < 0.5 被舍成背景 | 多通道报 bad_input；直接比较原值 ≠ 0（NaN 不算） | `test_image_ops.cpp` 的区域统计用例加两行 |
| 低 | 宿主注入不完整的图像报 `internal` | 注入的（provided）报 `bad_input` | `test_flow.cpp` 的注入用例改了期望 |
| 低 | Rust 客户端注入图像不查像素长度，短了 core 越界读 | `RunImageInput::pixels_ptr`：不够长就给 null，core 报在那个节点上 | `bridge/src/execution.rs` 的 `encode_image_frame_matches_the_documented_layout` 加四行 |
| 低 | 主预览的状态遮罩条件比图像面板的挂载条件宽，少数情况下画面一片空白 | 两处条件对齐 | —— |
| 低 | 文档过时：image-plan §5.1 / §5.2 还是 40 字节头、没有原图宽高；`builtin_ops.cpp` 的注释还写着 channels 契约 | 改掉 | —— |

留到以后（记在这里，不在这次改）：
- ~~图像的逐通道统计在结果仓的锁里、每次取输出元信息都重算一遍，2000 万像素的图每次几十毫秒~~（阶段 4 已做：Data 里缓存一份，浅拷贝共用）。
- 拖框的 begin / commit 在拖动中途画布被卸掉时可能不配对（点云的 RoiLayer 同一写法）。
- 编辑器 `fetchLevel` 与 MCP `view_output_image` 的「超过 16 MB 分段取」没有测试覆盖（测试用的图都一段取完）。

## 已知毛刺

- **大图没有验过，实测有两处问题（2026-10-01，修法待定）。** 上面各阶段验收用的图最大 320×240（推理那条是 1280×6）。
  用 `test.make_image`（`LYFLOW_TEST_OPS=1`）出 5472×3648 的合成图，CLI 跑：
  - **预览不缩图像。** ADR-0011 的源头抽稀只认点云（`executor.cpp` 里 `asCloud()` 那一段），`--preview` 下每个节点仍是
    19,961,856 像素。`to_gray → blur → threshold(otsu) → morphology(open) → region_stats` 单算一趟约 200 ms
    （28 / 8 / 24 / 30 / 109 ms），已经超过交互清单 #33 的「拖动到渲染 < 100 ms」。
    预览超预算时的提示是「建议降低预览点数」，对图像链路无效。
  - **`image.find_circle` 没有上界。** 0.3 MP 0.7 s、1.2 MP 12.5 s、5 MP 超过 120 s、20 MP 跑了 6 分钟（27 分钟 CPU）没出结果。
    合成图是 `(3x + 5y + 60c) mod 256` 的斜条纹，边缘很密，接近霍夫找圆的最坏情况，真实零件图会快得多；
    问题在于没有上界，OpenCV 的调用中途也停不下来。ADR-0011 说不可取消的算子在预览下无碍，
    前提是输入已经抽到 20 万点，这个前提对图像不成立。按代码读，桥接层的抢占是同步的 cancel + join
    （`bridge/src/execution.rs` 的 `RunManager::start`），下一次运行要等它算完才开始；app 里没有实测。
- 一次 `LYFLOW_PACKS=gap;dts` 的 doctest 在构建脚本里报了 1 个失败（151 例跑了、101 例没跑到），没留下失败详情。
  之后同一构建连跑 4 次全量、单独跑 gap 的 `test_blocks.cpp` 20 次都是全绿，复现不了。先记在这里，再出现时保留完整输出。

## 数字

- doctest：默认 173 例；`LYFLOW_PACKS=dts` 181；`gap;dts` 263（阶段 1 +6，阶段 2 +8，阶段 4 +3）。
- cargo：lib 139（+1）。editor 77（在已有用例里加行，个数不变）；MCP 30（阶段 3 +1）。
- e2e：752 条断言、100 个分组（阶段 1–2 +5 条 +1 组，阶段 3 +6 条 +1 组，review 修正 +1 条；全量跑过，失败的 3 条是 KUN10 缺数据的已知项）；
  `e2e:http` 33（阶段 1 +1）。

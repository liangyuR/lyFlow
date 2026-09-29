# 图像数据域验收（阶段 1–2）

> 2026-09-29。计划 [image-plan.md](image-plan.md)，决定 [ADR-0026](adr/0026-image-data-domain.md)。
> 阶段 3（主预览图像模式、几何叠画、图像上拖 ROI、MCP 看图）与阶段 4（深度图 ↔ 点云）未做。

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

## 已知毛刺

- 一次 `LYFLOW_PACKS=gap;dts` 的 doctest 在构建脚本里报了 1 个失败（151 例跑了、101 例没跑到），没留下失败详情。
  之后同一构建连跑 4 次全量、单独跑 gap 的 `test_blocks.cpp` 20 次都是全绿，复现不了。先记在这里，再出现时保留完整输出。

## 数字

- doctest：默认 170 例；`LYFLOW_PACKS=dts` 178；`gap;dts` 260（阶段 1 +6，阶段 2 +8）。
- cargo：lib 139（+1）。editor 77、MCP 29（在已有用例里加行，个数不变）。
- e2e：745 条断言、99 个分组（+5 条、+1 组；全量跑过，失败的 3 条是 KUN10 缺数据的已知项）；`e2e:http` 33（+1）。

# 图像数据域（Image）计划 —— 调研与待定决定

> 2026-09-29。状态：**已定，实施中**。§0 全按建议值；§6 三问的答复：
> Q1 暂无具体的业务图像场景，先按 §4 的通用集做、用合成图测；Q2 选 (a)，几何值带 `unit`；Q3 `std-image` 默认开。
> 阶段 1 的 e2e 用 core 的测试算子 `test.make_image`（`LYFLOW_TEST_OPS=1`，与 `test.param_showcase` 同一条路）出图，
> 不必等 OpenCV 包。决定写成了 [ADR-0026](adr/0026-image-data-domain.md)。
> 来源：roadmap「M5 之后 — 外延」第一条；边界规则照 [ADR-0005](adr/0005-pcl-boundary.md)，
> 取数方式照 [ADR-0019](adr/0019-output-tensor-and-indices-over-abi.md)。

目标：**图上能流图像**。能读图，能用 OpenCV 处理，双击连线就能看，结果能喂给 `ml.onnx_run`，
能和点云在同一张图里互转。顺带检验「数据模型是否通用」：如果加一个数据域要改的地方超出 §2 的清单，
就说明模型哪里焊死了，要记下来。

---

## 0. 待定的决定（建议值 + 理由）

| # | 决定 | 建议 | 一句话理由 |
|---|---|---|---|
| I1 | 图像是不是一个新的 `Data::Kind` | **是**，不复用 `Tensor` | `Tensor` 只有 float32。一张 500 万像素的 RGB 图用 u8 存是 15 MB，转成 float 就是 60 MB。另外通道语义、位深、「这是一张图」这件事都要靠类型表达：特征图不该不经转换就接进 `image.threshold` |
| I2 | 端口类型分几个 | **只有一个 `Image`**。通道数、位深是运行时属性，同点云的可选通道一个道理 | 分成 `ImageGray` / `ImageRGB` 要么逼出隐式转换（`castableTo`），要么让每个算子写好几份。要求单通道的算子用端口契约声明，见 I7 |
| I3 | 内存布局 | 行主序、**交错存储**（HWC）、**行紧排**（stride = 宽 × 通道 × 字节，不留行填充）；位深 `u8 / u16 / f32`；通道 1 / 3 / 4，**顺序 RGB(A)** | 交错是 OpenCV、PNG、canvas 的共同形状。行紧排让切片退化成「一段连续字节」，与张量同一套取法。RGB 而不是 OpenCV 的 BGR：公共模型只反映本项目自己的抽象（ADR-0005），BGR 转 RGB 只在 adapter 里做 |
| I4 | 像素缓冲的所有权 | `std::shared_ptr<const uint8_t>`（**别名构造**），外加 `width / height / channels / depth` | adapter 可以把 `cv::Mat` 的引用计数「藏」在 deleter 里，把结果**零拷贝**交给 core，core 却不用认识 OpenCV。`std::vector` 做不到这一点，每出一次 OpenCV 都得多拷一份 |
| I5 | 取数：抽稀还是切片 | **两级**。`level = 0` 是原图，按行切片、零拷贝借用；`level = k` 是 2^k 倍**均值缩小**，core 里现算（不依赖 OpenCV） | ADR-0019 说抽稀过的图是「另一张图」。那说的是**隔行取样**；均值缩小是「离远了看同一张图」，适合全图概览。像素级细节放大到 1:1 时再按可视区域取 level 0 的行 |
| I6 | 2D 几何类型（Box2D / Circle2D / …）的单位 | **待你决定**（见 §6 Q2）。倾向于：图像算子输出**像素**，值里带 `unit: "px"`，缺省是 `"m"` | `builtin_ops.cpp` 的注释写死了「与点云同单位（米）」。图像算子不可能凭空给出米。另开一套 `*2DPx` 类型会让类型表翻倍 |
| I7 | 端口契约 | **复用 `shape`**：图像按 `[高, 宽, 通道]` 读，「要灰度图」写 `[-1, -1, 1]`；`finite` 放开给 `Image` 的 f32。不开第五种键（实施时改的，原提案是新键 `channels`） | ADR-0024 刻意只有四种键；图像的形状本来就是 HWC，与张量同一种写法。契约在输入绑定时检查（运行期），报 `contract_violation` 并带期望与实际 —— 不是连线时的静态检查 |
| I8 | OpenCV 放哪 | 新包 **`packs/std-image`**，**默认开**，找不到 OpenCV 就 FATAL（与 std-pointcloud 对 PCL 的态度一致）。`opencv2/` 头只出现在包内部 | ADR-0005 的四条规则原样照搬（§3）。本机 vcpkg 已有 `opencv4 4.5.5`（`C:\vcpkg\installed\x64-windows`，core / imgproc / imgcodecs 等都在），现有的 toolchain 能直接 `find_package(OpenCV CONFIG)` |
| I9 | 与 `Tensor` 的关系 | 两个显式转换节点：`image.to_tensor`（归一化、mean/std、HWC → NCHW）和 `tensor.to_image` | 张量怎么当图像看，edge-peek 已经解决了。图像进推理需要的是**可见的预处理参数**，不是隐式转换 |
| I10 | C ABI | v14 → **v15**，纯增量：`lyflow_output_image` + `lyflow_image_view_free`，外加宿主注入图像（§2「宿主注入」） | 与 v8 加张量同一种增量，老符号不动。注入要一起做，不然业务服务只能先把图写成文件再让 `io.load_image` 去读 |

---

## 1. 数据模型

```cpp
// core/include/lyflow/data.h —— 不出现任何 OpenCV 头
enum class PixelDepth : uint8_t { U8 = 1, U16 = 2, F32 = 4 };  // 值就是每通道字节数

struct Image {
  int32_t width = 0, height = 0;
  int32_t channels = 0;          // 1 / 3 / 4；3、4 是 RGB(A)
  PixelDepth depth = PixelDepth::U8;
  std::shared_ptr<const uint8_t> pixels;  // 行紧排、交错；别名构造，所有者可以是任何东西

  size_t rowBytes() const;       // width * channels * depth
  size_t byteSize() const;       // rowBytes * height
  bool consistent() const;
  static Image allocate(int32_t w, int32_t h, int32_t c, PixelDepth d);  // core 自己的算子与测试用
};
```

- `Data::Kind::Image` **追加在 `Bundle` 之后**（data.h:146，注释要求不挪老序号）。工厂函数与 `asImage()` 仿 `tensor` 成对加上。
- `data.cpp` 里每个按 kind 分派的 switch 都要加一支：`byteSize`、`elementCount`（= 像素数）、`valueJson`、`kindFromTypeName`、`typeNameFromKind`。
- `valueJson`：`{kind:"image", width, height, channels, depth, min[], max[], mean[]}`，**逐通道**、只统计有限值（同 Tensor 的规则）。像素本身不进 JSON。
- 暂**不带**相机内参或像素到世界的变换。深度图转点云时，内参作为那个算子的参数（§4 第三组），不进数据模型。等出现第二个需要内参的算子再议。

## 2. 要改的地方（「加一个数据域」的真实清单）

这一节本身就是那次检验：凡是下面没列出来、做的时候却不得不改的地方，都记进验收文档，当作「模型焊死」的证据。

| 层 | 位置 | 改什么 |
|---|---|---|
| 类型表 | `builtin_ops.cpp` `registerBuiltinTypes` | `PortType{"Image", 颜色, {}, doc}` |
| 契约 | `registry.cpp:533/539`、`contract.cpp` | `shape` 与 `finite` 放开给 Image（shape 按 `[高, 宽, 通道]`）；schema 与 ADR-0024 各补一句 |
| 宿主注入 | `c_api.h:114` `lyflow_run_input`、`c_api.cpp:257` | 宿主（阶段 B 的业务服务）要能把相机图像直接注入源节点。`lyflow_run_input` 是**数组元素**，改它的大小等于破坏 ABI，所以另开 `lyflow_run_image_input`（node、port、宽高、通道、位深、`row_bytes`、像素指针），挂在 run options **末尾新加的** `image_inputs / image_input_count` 上（同 v7 加 `inputs` 的做法） |
| 注入数据的摘要 | `executor.cpp:1558` | 把 Image 的像素字节加进摘要。顺带修一个已有漏洞：云的 **normals / rgb 不进摘要**，同样的 xyz 换一份颜色注入会命中旧缓存 |
| 磁盘缓存 | `disk_cache.cpp` | `kKindImage = 3`，编码 / 解码，`persistable()` 放开。老文件里没有 3，所以 `kFormatVersion` **不用升** |
| C ABI | `c_api.h` / `c_api.cpp` | §5.1；ABI 号改四处：`c_api.h:5`、`lyflow-client/src/lib.rs:23`、`core/CMakeLists.txt:347`，`client.hpp` 自动跟随。顺手把 `embedding.md:505` 过时的 `11` 改掉 |
| Rust 客户端 | `crates/lyflow-client` | `ImageViewRaw`、符号加载、`Core::output_image`、安全包装 |
| 桥接 | `bridge/src/execution.rs`、`commands.rs`、`host.rs` | `IMAGE_MAGIC 'LYIM'` 与 `encode_image`；命令 `get_output_image`（按 16 MB clamp） |
| CLI | `bridge/src/cli.rs` `cmd_dump` | 输出是 Image 时写 `.png`（u8 / u16），或写原样的 `.lyim` 载荷（给 HTTP 桩用，见 §5.3） |
| HTTP | `docs/http-transport.md`、test-server | `GET /lyflow/runs/:runId/images/:nodeId/:port?level=&row=&rows=`，布局表 |
| 编辑器 | `types/execution.ts`、transport 三份、`viewRule.ts`、`EdgePeek.tsx`、`Inspector.tsx`、`compareDiff.ts:239` | 解码器、取数方法、视图选择、新 `ImageView`、摘要文案 |
| MCP | `cloud.ts`、`http.ts`、`tools.ts:511/975/988` | 解码、`summarize_output` 分支，以及 §5.5 的图片返回 |
| 硬编码类型名 | `core_ffi.rs:225` | 测试断言（`app/public/manifest.dev.json` 是 `pnpm core:dump` 生成、不进 commit 的，不用手改） |
| 文档 | `op-packs.md:419` 端口类型表、`operator-manifest.md`、`architecture.md` | |

## 3. OpenCV 边界（ADR-0005 照搬）

1. `core/include/` 下零 OpenCV 头；公共模型就是 §1 的 `lyflow::Image`。
2. `cv::` 只出现在 `packs/std-image/` 的算子源文件和私有的 `include/lyflow_cv/adapter.h` 里，外加一个 OpenCV 专用的 PCH。
3. 进出 OpenCV 一律经 adapter：
   - `toMat(const Image&)`：零拷贝，建一个指向同一块内存的 `cv::Mat` 头。**只读**：算子要原地改就先 `clone()`；
   - `fromMat(cv::Mat)`：零拷贝，用别名 `shared_ptr` 持有 Mat 的引用计数；不连续的 Mat 先 `clone()` 一次；
   - BGR ↔ RGB 只在 `imread` / `imwrite` 这一层转，其余算子不关心通道顺序。
4. 对应 PCL 的「尽量出 Indices」：图像算子**尽量出掩膜或几何**（Box2D / Circle2D / Measurement），少出「又一张 RGB 图」。

构建：`lyflow_op_pack(NAME std-image DEFAULT ON …)`，`find_package(OpenCV 4 CONFIG REQUIRED COMPONENTS core imgproc imgcodecs)`，
照 std-ml 的写法把 `opencv_core / imgproc / imgcodecs` 的 DLL 拷到 `bin/`，`install-lyflow.ps1` 同步。
注意：**默认开，意味着每台构建机都要先 `vcpkg install opencv4`**，README 的前置依赖要加一行。

## 4. 第一批算子（`std-image`，约 12 个）

原则：先够组成三条真实的链路，不追求覆盖 OpenCV。

| 组 | 算子 | 输出 |
|---|---|---|
| IO | `io.load_image`（png / jpg / bmp / tif，参数 `mode`：原样 / 灰度 / 彩色；u16 原样保留）、`io.save_image` | Image |
| 基础 | `image.to_gray`、`image.resize`、`image.crop`（像素矩形参数，`semantic="roi"`）、`image.blur`（高斯 / 中值）、`image.normalize`（位深转换 + 线性拉伸） | Image |
| 分割与测量 | `image.threshold`（固定 / Otsu / 自适应，输入契约 `shape: [-1, -1, 1]`）、`image.morphology`、`image.find_circle`（Hough，取最强一个）、`image.region_stats`（掩膜内的均值、面积） | Image 掩膜 / Circle2D / Measurement |
| 跨域 | `image.to_tensor`、`tensor.to_image`（I9）；`cloud.from_depth`（u16 深度图 + fx fy cx cy + 深度比例 → PointCloud，可选再给一张彩色图填 rgb）、`cloud.to_depth_image`（有序云或投影 → 深度图） | Tensor / Image / PointCloud |

三条验收链路：

- **纯 2D**：`load_image → to_gray → threshold → region_stats`，得到一个带上下限判定的 Measurement；
- **推理**：`load_image → resize → to_tensor → ml.onnx_run → tensor.to_image`；
- **跨域**：`load_image(深度 u16) → cloud.from_depth → filter.passthrough → …`。一张图里点云和图像两个域都有，这才是「数据模型通用」的真实检验。

### 4.1 阶段 4 的约定（深度图 ↔ 点云，2026-09-30 定）

| # | 决定 | 理由 |
|---|---|---|
| D1 | **相机坐标系按 OpenCV 针孔约定**：x 向右、y 向下、z 向前（离开相机），单位米。`x = (u − cx)·z / fx`，`y = (v − cy)·z / fy`，`u, v` 是像素中心的整数坐标 | 与 OpenCV / 绝大多数相机 SDK 的内参同一套，拿到手的 fx fy cx cy 不用换算；点云要换到机器人或世界坐标系就接 `transform.apply` |
| D2 | **内参是算子参数**（fx fy cx cy，像素），不进数据模型 | ADR-0026「不做」：出现第二个需要内参的算子前不加相机类型。两个算子各带一份，参数可以提升成图参数共用 |
| D3 | **深度值 × `depthScale` = 米**。u16 默认 0.001（毫米深度图，最常见）；f32 深度图一般是米，设 1 | 相机 SDK 的深度单位五花八门，一个乘数就能覆盖 |
| D4 | **无效深度跳过**：0、非有限值、落在 `[minDepth, maxDepth]` 外（上限 0 = 不限）的像素不出点。输出是无序点云 | core 的点云是无序的（ADR-0005 背景）；有序云要改数据模型，不在这次 |
| D5 | 可选 `color` 输入：与深度图同样大的 RGB(A) 图，按像素给点上色（u8 原样；u16 取高 8 位；f32 按 0..1 映射） | 深度相机大多带一张对齐好的彩色图；不对齐的要先配准，那不是这个算子的事 |
| D6 | `step`：每隔几个像素取一个（1 = 全取） | 百万像素的深度图出百万个点，调参时先稀疏看 |
| D7 | `cloud.to_depth_image`：按内参把点投影回去，一个像素落多个点取**最近的**（z 缓冲），落不到点的像素是 0；z ≤ 0 与投影到图外的点丢掉。输出 u16（÷ depthScale 四舍五入）或 f32（米） | 与 from_depth 互逆：同一组内参来回一趟，有效像素逐个相等（验收） |
| D8 | 两个算子放在 `std-image`（图像域），但**不用 OpenCV** | 包的依赖已经在了；算法就是几行针孔公式 |

另外两件顺带做的：
- **图像统计缓存**（PR #1 review 留下的一条）：`Data::valueJson` 对图像只算一次，存在 Data 共享的缓存里。深度图正是大图，每次取输出元信息都整图重算会拖慢结果仓。
- **推理链路验证**：用桌面 DTS 文件夹里的 `v12s0.onnx`（gap 的剖面模型，`[N, 6, 1280] → [N, 8, 1280]`，不是视觉模型）走一遍
  `图像 → image.to_tensor（CHW）→ ml.onnx_run → tensor.to_image`，验证的是链路通、形状对，不是视觉语义。

## 5. 取数与显示

### 5.1 C ABI（v15）

```c
/* 实施时的定义以 core/include/lyflow/c_api.h 为准；比最初的提案多了原图宽高 */
typedef struct {
  uint32_t width, height;             /* 这一级（level）的完整尺寸，不随切片变 */
  uint32_t channels, depth;           /* depth：每通道字节数 1 / 2 / 4 */
  uint32_t level;                     /* 实际给出的级别（要得太大时收到长边 1 像素那一级） */
  uint32_t full_width, full_height;   /* 原图（level 0）的宽高 */
  uint32_t row_offset, row_count;
  uint32_t row_bytes;
  const uint8_t* pixels;              /* 指向 row_offset 那一行 */
  void* handle;
} lyflow_image_view;

LYFLOW_API int lyflow_output_image(const char* run_id, const char* node_id, const char* port,
                                   uint32_t level, uint32_t row_offset, uint32_t row_count,
                                   lyflow_image_view* out);
LYFLOW_API void lyflow_image_view_free(lyflow_image_view* view);
```

- `level = 0`：`handle` 持 `Data` 的浅拷贝，`pixels` 直接指进结果仓里那一份（同张量，零拷贝）。
- `level > 0`：core 用 2^level 的块均值现算一份，放进 `handle`（同点云预览，复制）。u8 / u16 四舍五入，f32 跳过非有限值。
- `row_offset` 越界返回成功、`row_count = 0`；`row_count = 0` 表示读到底；`<port>.<field>` 的 Bundle 字段照旧支持。返回码同 `lyflow_output_tensor`。

### 5.2 二进制载荷 `LYIM`

`magic u32 | width u32 | height u32 | channels u32 | depth u32 | level u32 | full_width u32 | full_height u32 | row_offset u32 | row_count u32 | row_bytes u32 | 保留 u32`
（48 字节头，逐项的表见 [http-transport.md](http-transport.md)「图像」），后面接像素、补齐到 4 字节。
头长 48，所以 u16 / f32 像素天然对齐，前端可以零拷贝建 `Uint16Array` / `Float32Array`（http-transport.md 的共同规则）。

### 5.3 HTTP 桩

HTTP 桩（test-server）张量那条路给的是 501，因为桩没有常驻结果仓。
图像可以不同：`lyflow dump` 对 Image 输出写一个原样的 `.lyim`，桩读出来切片返回。这样桩能覆盖图像，`e2e:http` 也能测到。

### 5.4 编辑器

- **连线查看器**新视图 `image`：`components/peek/ImageView.tsx`。
  - 先取一级概览，level 取「长边不超过 2048」的那一级；可以平移、缩放，放大到 1:1 时按可视行取 level 0。
  - 鼠标悬停显示 `(x, y)` 和该像素的原始值。
  - u16 / f32 复用 `TensorView` 的 `normalizer`、色带和自动归一化，抽到 `lib/imageNorm.ts` 给两个视图共用。
  - `viewRule`：`Image → ["image", "value"]`；PNG 导出走现成的 `canvasView()`。
- **主预览**（Viewer3D 的模式下拉框）加「图像」模式。选中 Image 输出的节点时用它，并且把**同一条链路上的 2D 几何**（像素单位的那些，I6）叠画在图上，与「2D 剖面」叠画几何是同一套做法。
- **图像上拖 ROI**（`image.crop` 的矩形参数）：放在第三阶段，复用 roi 框编辑的交互，底图换成图像。
- Inspector：`1920×1080×3 u8 · 均值 (…)`。

### 5.5 MCP

- `summarize_output`：Image 给尺寸、通道、位深和逐通道的 min / max / mean（直接用 valueJson）。
- **新工具 `view_output_image`**：返回一张缩小的 PNG，走 MCP 的 `image` 内容类型，让 agent 真的能**看见**中间结果。
  - PNG 编码用 Node 自带的 `zlib.deflateSync` 手写，约 50 行，不加依赖；
  - u16 / f32 按自动归一化转成 u8；
  - 这是 MCP 第一次返回非文本内容，`mcp.md` 与 ADR-0021 各补一句。

## 6. 需要你拍板的问题

- **Q1 有没有真实的图像场景在推这件事？** 例如 DTS / gap 的 2D 相机，或 3D 相机本来就吐的深度图、彩色图。
  - 有：第一批算子按它挑，跨域那组（深度图 ↔ 点云）提到第二阶段；
  - 没有：按 §4 的通用集做，拿合成图测。
- **Q2 2D 几何的单位**（I6）。三选一：
  - (a) 值里带 `unit: "px" | "m"`，混用时由消费方报错（建议）；
  - (b) 另开 `Box2DPx` 等类型；
  - (c) 图像算子也输出米，需要一个全局的「像素尺寸」参数。
  - 这一条决定 Viewer 怎么叠画，也决定图像几何会不会被误接进点云算子。
- **Q3 `std-image` 默认开？** 默认开的代价是所有构建机都要装 OpenCV（vcpkg 编一次要很久）。另一种做法是默认关，e2e 用 `LYFLOW_PACKS=std-image` 显式打开，与 gap 包一样。我倾向**默认开**：它是标准包，而不是业务包。

## 7. 阶段与估时

| 阶段 | 内容 | 估时 | 验收 |
|---|---|---|---|
| 1 ✅ | §1 数据模型 + §2 清单（不含 OpenCV）+ ABI v15 + 载荷 + 连线查看器 `ImageView`。测试源用 core 测试里的合成算子 `test.make_image` | 3–4 天 | doctest（取视图、切片、level、越界、磁盘缓存往返、摘要）；cargo（载荷布局）；e2e `peek` 加一组「双击看图、悬停读值」；`e2e:http` 走 `.lyim` |
| 2 ✅ | `std-image` 包 + adapter + §4 前三组算子 + `image.to_tensor` / `tensor.to_image` | 3–4 天 | 包内 doctest（adapter 往返、BGR/RGB、零拷贝持有）；纯 2D 与推理两条链路在 CLI 跑通；e2e 一条「读图 → 二值化 → 看」 |
| 3 ✅ | 主预览图像模式 + 几何叠画 + 图像上拖 ROI + MCP 图片返回 | 3 天 | e2e `m8b` 拖框组加一例；MCP 冒烟调一次 `view_output_image` |
| 4 ✅ | 跨域：`cloud.from_depth` / `cloud.to_depth_image` | 2 天 | 跨域链路跑通；深度图转点云再转回，与原图逐像素对得上 |

每个阶段各自提交；阶段 1 结束时写 ADR-0026（Image 数据域与取数方式），并照惯例写验收记录：[image-acceptance.md](image-acceptance.md)（阶段 1–2 已写）。
测试照 [testing.md](testing.md)：能在 doctest 测的不进 e2e，改完同步地图与数字。

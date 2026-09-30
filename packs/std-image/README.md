# packs/std-image

图像域的标准算子包（[docs/image-plan.md](../../docs/image-plan.md) 阶段 2，[ADR-0026](../../docs/adr/0026-image-data-domain.md)）。
默认开，依赖 vcpkg 的 OpenCV 4（只用 core / imgproc / imgcodecs）：

```powershell
C:\vcpkg\vcpkg.exe install opencv4:x64-windows
```

## 边界（照 ADR-0005）

1. `core/include/` 下零 OpenCV 头；公共数据模型是 core 的 `lyflow::Image`（行主序、通道交错、行紧排、RGB(A)）。
2. `cv::` 只出现在这个包的 `ops/`、`src/` 与私有的 `include/lyflow_cv/`。
3. 进出 OpenCV 一律经 `lyflow_cv/adapter.h`：
   - `view` 零拷贝、只读；
   - `fromMat` 零拷贝交给 core（别名 `shared_ptr` 持有 Mat）；
   - BGR ↔ RGB 只在 `io.load_image` / `io.save_image` 里转。
4. 尽量出掩膜、几何与测量，少出「又一张彩色图」。几何是**像素坐标**，`unit = px`（image-plan Q2）。

文件读写不用 `cv::imread` / `imwrite`：它们走窄字符串路径，中文路径打不开。
这里用 `std::filesystem` 读写字节，再交给 `imdecode` / `imencode`。

## 算子

| 算子 | 做什么 |
|---|---|
| `io.load_image` / `io.save_image` | PNG / JPEG / BMP / TIFF；u16 保留，f32 只能存 TIFF |
| `image.to_gray` | RGB(A) → 单通道（BT.601），已是灰度的原样传下去 |
| `image.resize` | 按倍数或尺寸；缩小默认区域平均 |
| `image.crop` | 像素矩形，或接一个像素单位的 `Box2D` |
| `image.blur` | 高斯 / 中值 / 均值 |
| `image.normalize` | 位深转换：按通道拉满量程，或 × scale + offset |
| `image.threshold` | 固定 / Otsu / 自适应 → u8 掩膜；输入契约 `shape: [-1, -1, 1]` |
| `image.morphology` | 腐蚀 / 膨胀 / 开 / 闭 |
| `image.find_circle` | 霍夫找圆，取最强的一个 → `Circle2D`（px） |
| `image.region_stats` | 掩膜内的均值、面积、外接框（px，可接 `image.crop`） |
| `image.to_tensor` / `tensor.to_image` | 推理前后的显式转换：scale、mean/std、NCHW 等布局 |
| `cloud.from_depth` / `cloud.to_depth_image` | 深度图 ↔ 点云（针孔内参，x 右、y 下、z 前，米）：无效深度不出点，可选彩色图上色；投回去按 z 缓冲取最近。同一组内参、同一个 depthScale 时两者互逆（[image-plan.md](../../docs/image-plan.md) §4.1）。不用 OpenCV |

测试在 `tests/test_image_ops.cpp`，图一律现造（合成渐变、`cv::circle` 画的圆），仓库不进图片。

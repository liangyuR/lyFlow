# BarcodeDetector / QrCodeRefine 分析与实现

## 外部参考的实际意图

只参考 `xyz-vision-lib` 的两份节点 cpp/hpp 和 `property_settings.yml` 的接口语义，没有复制其专有函数体。`BarcodeDetector` 以灰度图和类型筛选调用外部 `xyz_imgproc` 解码器，只返回字符串；该底层实现没有包含在所给仓库中。原接口不能区别无目标和空字符串，也不提供多码、四角、逐目标错误或原始字节。

`QrCodeRefine` 的意图是对已经裁正、格数已知的二维码计算模块均值，按局部亮暗范围阈值重绘，不是寻找二维码、推断版本或恢复被遮挡的数据。参考代码在处理ROI后仍从原输入尺寸/像素采样，默认16格并非标准QR Model2格数，且只留一格静区。LyFlow独立实现明确裁正区域、标准格数和四格静区，并用真实解码结果反馈增强效果。

## 固定后端与语言边界

使用官方当前稳定版 [ZXing-C++ v3.1.1](https://github.com/zxing-cpp/zxing-cpp/releases/tag/v3.1.1)，固定修订 `287c85df6f961c8efbfb5ffd736cd9457b8b890e`。官方release ZIP为4,712,272字节，SHA-256 `c914877d2598e6c748725e9bc0dad9b667710537c6b91a3d8b8a7214834ee1d5`，与官方API资产digest一致。使用完整release资产，避免上游提醒的自动源码归档缺失子模块问题。

`packaging/barcode-deps.json` 固定来源、归档和补丁；`scripts/fetch-barcode-deps.py` 复用既有安全解包/哈希/补丁工具，但不更改匹配依赖清单或准备流程。归档先校验字节数/SHA，源码目录以逐文件库存校验。签名包含清单、准备器、公共源码工具和补丁哈希；有变化须使用干净输出目录，禁止补写旧stamp。CMake仅离线验证，不在configure中下载。

`core/cmake/barcode-deps.cmake` 的 `lyflow_configure_barcode_cpu()` 提供静态 `lyflow_barcode_backend`。ZXing与私有wrapper使用C++20，节点及core消费端保持C++17；接口只穿过自有C++17结构。启用全部reader，关闭writers、Zint、C API、实验API、上游测试/示例。没有新增ZXing运行时DLL，也不依赖OpenCV的QUIRC。

主代码Apache-2.0。实际reader还编入BSD-3-Clause的libzueci及其UTF-8状态机MIT许可通知，完整附加通知在 `packaging/licenses/barcode-third-party.txt`，发包同时复制上游LICENSE和来源/补丁说明。Zint未编译或链接，不把它的许可作为reader运行库许可。新增内部契约测试通过 `lyflow_add_barcode_contract_tests(testTarget)` 以私有C++20 OBJECT target接入，不能因此升级整个core测试目标语言标准。

### 合法零字节QR

原始3.1.1在 `DecoderResult::isValid()` 与 `BarcodeData::isValid()` 要求非空字节，丢弃了已经通过QR格式、版本、Reed–Solomon纠错和位流解析的合法空内容。独立探针用OpenCV5.1生成version1/MODE_BYTE零字节QR，直接解码得到 `bytes=0 / error空 / version=1 / symbology=]Q1`，原始有效性却为false；同图损坏144模块后返回ChecksumError。

固定补丁只在QR Model2的外层 `Decode` 成功出口设置默认false的空QR成功标记，前提是有数据码字、所有RS成功且位流无错误。标记经MatrixBarcode保存；BarcodeData只允许该QR格式且无错误时交付空载荷。默认对象、其他格式、失败结果和未通过外层检查的空解析对象不会放开。三个修改文件都有修改通知；没有改格式名称、向载荷塞字符或全局允许空解码。

## image.barcode_decode

输入 `image` 为u8/u16/f32灰度、RGB或RGBA；RGB顺序复用主线，f32所有通道须有限且在0..1。内部归一化为u8用于解码。可选 `roi` 为像素Box2D、`region` 为相同frame/px的Pose2D旋转矩形，两者互斥；ROI无图像交集报错。ROI外设为白色，结果四角还必须位于选择区内。

| 参数 | 默认值 / 范围 | 含义 |
| --- | --- | --- |
| frame | image / 1..256字节 | 结果参考帧 |
| formats | Any | Any、Linear、Matrix或公开格式枚举 |
| tryHarder | true | 增加扫描工作 |
| tryRotate / tryInvert / tryDownscale | true | 旋转、反色、多尺度搜索 |
| maxSymbols | 128 / 1..128 | 检测项预算，包含错误候选 |

输出 `instances` 为 `vision.Poses2D.v1`，仅包含有效解码，ID对应 `detections.items[].id`，`pickable=false`。四角转最小外接旋转矩形；没有将解码成功写成识别概率。1D位置可能只有扫描线高度，Pose最小一工作像素高度用于表示，不是条码真实高度或抓取依据。真实四角证据保留在记录中。

`detections` 为 `image.Barcodes.v1`：

```text
{frame, unit:"px", imageSize:[原图宽,原图高], backend, backendPatchRevision,
 textEncoding, items:[
  {id, valid, format, text, bytesHex, contentType, hasECI, error,
   inverted, mirrored, lineCount, corners:[TL,TR,BR,BL], orientationDegrees}
 ]}
```

坐标手动恢复原图像素；Record不会由引擎缩放。Box2D由引擎转换至工作比例，Pose2D Record由节点显式转换。`text` 是ZXing按ECI或字符集推断转出的UTF-8；`bytesHex`无损保留原始内容，不用文本替代二进制证据。`valid`独立于文本是否为空。ZXing实际返回的QR格式名为QRCode，筛选支持QRCodeModel2；保留后端真实名称。

输出 `image` 为原生叠图，保持输入位深、通道、alpha、尺寸与scale，源图只读。无目标成功返回空集合，错误候选有明确error；非法输入、取消和预算拒绝不伪装为空成功。

## image.qr_refine

输入 `image` 支持相同图像类型。可选 `roi` 表示已经裁正且不含静区的模块矩形；或连接 `image.Barcodes.v1` 的 `detections`，利用选定标准QR的四角做透视裁正。两者互斥，后者校验frame/原图尺寸/有限四角和图内范围，多QR必须指定symbolId。没有选择输入时，整幅图被视为已裁好的模块区域，不自动猜测边界。

| 参数 | 默认值 / 范围 | 含义 |
| --- | --- | --- |
| frame / outputFrame | image / qr_refined | 源图与新图坐标系 |
| symbolId | 空 | 指定detections中的有效标准QR |
| moduleCount | 21 / 21+4k，k=0..39 | 已知QR版本边长，排除Micro/rMQR |
| modulePixels | 8 / 1..32 | 新输出每格像素，不随预览scale缩放 |
| quietZone | 4 / 4..16 | 静区格数 |
| polarity | dark / dark, light | 源码极性，输出统一黑码白底 |
| thresholdMode | localRange / localRange, otsu | 模块阈值算法 |
| localRadius | 3 / 1..32格 | 局部亮暗范围半径 |
| minCellContrast | 0.08 / 0..1 | 低于门限用全局阈值并计入不确定格 |

独立流程为透视裁正→面积缩小到真实模块格数→局部亮暗中点或全局Otsu→最近邻重绘→四格以上静区。每源边至少一个工作像素/模块，不把严重预览欠采样的图虚构成有效数据。局部阈值腐蚀/膨胀使用明确中心锚点。

输出 `image` 为u8单通道原生图，尺寸 `(moduleCount+2*quietZone)*modulePixels`，scale=1，表示新的栅格坐标系。输出 `feedback` 为 `image.QrRefinement.v1`，含源四角、输出到原图的3×3单应矩阵、源/输出frame、实际参数、不确定格数、最低局部对比度、before/after逐码内容及布尔解码状态。静区是新增图像，单应矩阵在静区只是几何外推，不表示采集到的源数据。

modulePixels描述符使用 `unit=px, absolute=true`，通过引擎既有绝对输出尺寸机制保持新图scale=1；不能只在compute里赋scale=1后让执行器再次继承源图预览scale。源四角和feedback矩阵仍按原有规则恢复原图坐标。永久调度器回归用2200²真实QR像素触发Preview源头缩小，串接前解码→增强→后解码，核对232²新栅格、后解码imageSize/四角及outputToSource配对，不以仅compute直调代替执行器验证。

before采用同样裁正几何和静区，但尚未模块均值阈值重绘；after才使用重绘结果，避免把裁正或加静区的收益误算成阈值收益。`improved`仅指before无有效解码而after有有效解码，不是图像质量分或纠错保证。解码失败仍交付增强图和明确反馈；不会猜测被遮挡内容。

## 资源、取消和验证边界

输入≤8Mi像素、≤128MiB、宽高≤65535；预计图像工作峰值≤512MiB，输出也≤8Mi像素。最多128项，每码原始字节≤8KiB、文本≤32KiB；实际Record按主线 `values::boundedJson` 校验1MiB总量、64KiB单字符串、深度16和4096集合项。检测请求maxSymbols+1，实际129码反例验证越限报resource_limit，不静默截断。大型像素始终使用Image，不放入JSON/base64。

显式循环中检查取消；ZXing/OpenCV单次内部阶段不可抢占，在调用前后检查，不承诺硬超时或固定取消延迟。校验和资源错误在输出提交前失败。

永久节点测试使用独立生成的OpenCV QR Model2与标准Code39测试图，覆盖中文、合法零字节、真实1D、旋转/多码、128/129边界、三位深RGBA只读/alpha、ROI及frame错误、巨大坐标、空白/非法f32、透视裁正、反色、标准格数、增强前失败后成功。QR内部测试保留默认DecoderResult/BarcodeData、其他格式不能借用空QR标记、144模块损坏、16个随机模块矩阵及错误尺寸反例。源码准备测试覆盖离线补丁/库存往返、缺摘要、篡改源码/补丁/协议、归档完整性和路径穿越。

这些是软件夹具验证，不能推出全部编译格式、真实产线标签、反光、曲面、严重遮挡、误识别率、相机/PLC、吞吐或长期运行已验收。完整CLI/C ABI/安装后SDK及CI证据由本阶段统一交付门禁记录。

独立Windows MSVC探针实际编译固定ZXing读取器、私有C++20包装层和生产C++17算子：9组测试、317条断言全部通过，无跳过；源码准备器5组测试全部通过。该探针仅用小型Registry注册替身连接生产数据/算子实现，不构成完整引擎调度或跨接口验收。证据保存在 `build/p4-barcode-probe/patched-build.log`、`patched-tests.log`、`preparation-tests.log`；原版与补丁版的合法空QR对照见 `empty-original.log`、`empty-patched.log`。

# OCR 语义分析与实现契约

## 参考边界

逐项阅读本机 `xyz-vision-lib` 的 `OCR.cpp`、`OCR.hpp`、`property_settings.yml` 的默认值和参数声明。给定目录没有 Git 元数据，也没有 `xyz_learning_bridge/ocr_rec.h` 的识别器实现。参考节点消费彩色图像及附带 ROI，输出字符串和叠图，参数包含私有 ZIP 模型、CPU/GPU、线程数、字符类型和识别阈值。本仓库未复制专有函数体，不声明兼容该 ZIP 或复现不可得的识别模型。

LyFlow 使用显式的检测、四边形、校正、方向分类和识别组合。原参考按字节过滤字符的行为不适用于 UTF-8；这里按完整字典符号拼接 Unicode，不把中文字节当字符。GPU、私有模型管理器和模型自动下载不隐藏在算子中。

## 节点与八节点组合

| 节点 | 输入 | 输出 |
|---|---|---|
| `image.ocr_preprocess` | image、可选像素 Box2D roi | input Tensor、`image.OCRMapping.v1` mapping |
| `ml.onnx_run`，`ocr-db` | input | prediction，唯一 DB 概率头 |
| `image.ocr_db_decode` | image、prediction、mapping | quads Tensor、`image.TextRegions.v1` regions、标准 Poses2D instances、image 叠图 |
| `image.ocr_crops` | image、quads、regions | recInput、clsInput、`image.OCRCrops.v1` layout |
| `ml.onnx_run`，`ocr-direction-2`、serial | clsInput | classification |
| `image.ocr_orient` | recInput、classification、layout | input、layout |
| `ml.onnx_run`，`ocr-ctc`、serial | input | prediction |
| `image.ocr_ctc_decode` | prediction、layout | `std.Value.v1` text、`image.OCR.v1` results |

检测预处理和解码使用同一原图，裁切也消费该原图；方向和识别通过现有 ONNX 执行器复用会话。方向模型类别固定为 0°、180°，不能连接任意分类头。识别模型是单 CTC 头，不能静默选择多头模型的第一个输出。图中模型路径和字典路径显式配置，文件由宿主或准备脚本提供。

没有检测目标时 DB 输出 shape `[0,4,2]` 的四边形和空元数据，crops 保留合法零批次 NCHW 形状。ONNX serial 验证配置后不调用 N=0 推理，返回 canonical empty Tensor `{0}`；orient 和 CTC 接受这一空输出，最终得到成功空字符串和空 items。非法字典、模型契约错误不会因为无目标而被掩盖。

## 坐标、裁切与排序

本版 OCR 明确消费 `Image.scale=1`，拒绝降采样图，避免把降采样丢失的笔画当作完整文字。节点 `previewable=false`，直接传入 scale>1 仍报告诊断。图像支持灰度、RGB、RGBA 的 u8/u16/f32；f32 的所有通道须有限且位于闭区间 `[0,1]`，使用相邻 float32 上界保持合法 1.0 可用，拒绝最小的 >1 值。

ROI 与原图取像素中心交集，至少 2×2 像素；无交集或非法单位报错。预处理按最长边上限等比缩放，并把各边舍入到最近的 32 倍数，恰好半格使用偶数舍入，最短边至少 32。两方向的实际缩放独立记录，坐标恢复使用 OpenCV resize 的半像素中心逆变换，不用名义缩放系数代替。

DB 接收唯一 float32 `[1,1,H,W]` 概率图，H/W 必须等于 mapping 的输入尺寸，所有概率位于 `[0,1]`。二值化后按外轮廓内部均值打分，用 `area × unclipRatio / perimeter` 距离扩张有限最小旋转矩形。此为明确的矩形 DB 类后处理，不声明等价于任意凹多边形的 Clipper offset，也不将多个输出头猜作概率图。四边形恢复到原图 ROI 后检查凸性和最短边。

四边形始终是原图像素 Tensor `[N,4,2]`，顺序 TL/TR/BR/BL；regions 只保留 id、score、frame、imageSize 等小数据。区域先按 top/left 稳定排序，再把 top 差不超过首区域半个最短边的区域按 x 排成一行。这是显式的水平阅读顺序，不声称解决复杂版面、竖排阅读顺序或双栏文档重建。排序后分配 `text-0` 等 id，quads、regions、instances、layout、results 按同一顺序绑定。

`geometryHash` 是 float32 little-endian 字节的 FNV-1a 配对摘要，用于发现四边形和小元数据混接，不能作为安全认证、图像身份或源码修订证明。Image/Tensor 没有原生图像 id，本版不伪造身份；调用方需连接同一原图，尺寸校验不能识别内容不同但尺寸相同的图。

每个四边形真实执行透视校正。默认把高宽比至少 1.5 的竖行逆时针旋转 90°，再归一化为高度 48；识别输入保持比例并在右侧填零，宽度取所有行的最大有效宽度和 minWidth。超过 maxWidth 报 resource_limit，不截断字符或偷偷压缩长行。方向输入宽度最多 192，长行按完整内容压缩用于方向判断；这与识别输入的等比保字行为不同。

方向校正只翻转识别张量每行的实际有效宽度，右侧 padding 始终留在右侧。结果保留 classifierLabel、实际应用的 orientation、orientationConfidence、rotated90；低于置信阈值不翻转，禁止重复应用同一 layout。源图和上游张量保持只读。

layout 的 recHeight、recWidth、validWidth 表示识别输入张量的像素列行，不是原图区域尺寸；results 保留这些处理记录用于审计。原图几何由 quads/instances 和 geometryHash 配对，不能用归一化 validWidth 当原图测量值。

instances 使用 `vision.Poses2D.v1`、px、图像 x 右 y 下角度约定，pickable=false。叠图保留源位深、RGB 顺序和 alpha；只绘几何框，不依赖当前禁用 HarfBuzz 的 OpenCV 文本渲染，也不把 Hershey 问号当作 Unicode 文字。真实 Unicode 通过 text/results 呈现。

## CTC 和过滤

字典是 UTF-8 文本，一行一个非空符号，原行序不重排；接受首行 BOM 和 CRLF，拒绝非法 UTF-8、重复、空条目或超过 256 字节的符号。blank 固定为 0，字典依次占后续类别；appendSpace=true 时末尾再加一个空格，字典已有空格则需显式关闭它。

输出布局显式选择 NTC 或 TNC。概率模式要求每行所有值在 `[0,1]` 且概率和误差不超过 0.001；logits 模式明确执行稳定 softmax，不凭数值猜测。批次必须等于 layout 条目数，类别数必须精确等于 blank+字典+可选空格，时间步 1..8192。CTC 先折叠相邻相同类别，再删除 blank；blank 隔开的相同字符保留。

逐行保留 rawText、字典符号均值置信、过滤后的 text 和 accepted。minCharacterScore 在 CTC 折叠之后过滤；随后按 UTF-8 codepoint 遍历每个符号，受限字符集仅保留相应 ASCII 字符，all 保留全部 Unicode。多 codepoint 符号如“中Ab1🙂”在数字模式得到“1”，不会把整个混合符号丢掉或破坏编码。五种模式对应参考中实际存在的数字、数字加小写、数字加大写、数字加字母、全部；参考 OCR 参数未声明 regex、中文专用类别或显式允许列表，本阶段不附加这些功能。minTextScore 仅控制该行是否进入合并文本，低分条目依然在结果中，可区别无检测、识别空白与低置信。分数是模型输出的算法质量，不是经过现场校准的正确率。

字典 externalKey 包含路径、文件大小与 mtime，改变文件使执行缓存失效。这沿用现有路径类算子约定，不把元数据指纹冒称为文件来源认证。

## 参数与失败边界

| 节点 | 参数 | 默认与范围 |
|---|---|---|
| preprocess | frame | image，1..256 UTF-8 字节 |
| preprocess | maxSide / allowUpscale | 960，32..2048 且为32倍数 / false |
| preprocess | channelOrder / mean / std | BGR / `[.5,.5,.5]` / `[.5,.5,.5]`；std 1e-6..1 |
| DB | pixelThreshold / boxThreshold | .3 / .5，像素阈值 .001...999，区域阈值 0..1 |
| DB | unclipRatio / minSide / maxRegions | 1.5 [0,5] / 3 [1,1024]原像素 / 128 [1,128] |
| crops | minWidth / maxWidth / rotateVertical | 320 / 2048，宽度32..2048 / true |
| crops | channelOrder | BGR；模型像素归一化 `(value-.5)/.5` |
| orient | minConfidence | .9，0..1 |
| CTC | dictionaryPath / appendSpace | 必须显式路径 / true |
| CTC | axisOrder / outputMode | NTC / probabilities |
| CTC | minCharacterScore / minTextScore | 0 / .5，0..1 |
| CTC | characterSet / separator | all / 换行；分隔符至多16字节 |

图像最多 8Mi 像素或128MiB，Tensor 最多64Mi个 float32 值。DB 最多4096外轮廓、4Mi轮廓点、累计64Mi候选评分像素；合格区域超过 maxRegions 明确报错，不以候选次序截断目标。裁切累计最多16Mi像素，包含识别/方向批次及图像的预计内存上限512MiB。字典最多4MiB和65534符号；逐项及合并文本遵守64KiB，Record 遵守1MiB/深度16等现有界限。超预算不返回半批结果。

算子在入口、长循环、区域间和最终提交前检查取消；OpenCV 单次滤波/透视调用内部不承诺可抢占。模型执行沿用 ONNX 节点的阶段边界和 serial 批次取消约定，没有隐藏工作进程或固定取消毫秒承诺。

## 固定公开模型和许可

真实 CPU 验证采用 RapidAI 官方 v3.10.0 注册资产。该 tag 对应提交 `bd941dae09da793e76c6ba15c7aaf40f8424232a`，不是直接依赖可变 main。来源：[官方固定注册表](https://github.com/RapidAI/RapidOCR/blob/bd941dae09da793e76c6ba15c7aaf40f8424232a/python/rapidocr/default_models.yaml)、[官方模型许可声明](https://rapidai.github.io/RapidOCRDocs/main/en/model_licenses/)、[PaddleOCR Apache-2.0 许可](https://github.com/PaddlePaddle/PaddleOCR/blob/dab3fe35379033fdcb2d0e9572fac0b36c9a9ebf/LICENSE)。模型由 RapidAI 明确声明为 Apache-2.0 的 PaddleOCR 转换产物；模型许可不能自动覆盖另外下载的字体。

| 资产 | 字节数 | SHA256 |
|---|---:|---|
| PP-OCRv6_det_small.onnx | 9929594 | `090f04abcd9d9a7498bc4ebf677e4cb9bdce1fe4197ddb7e529f1ef44e1ff94f` |
| ch_ppocr_mobile_v2.0_cls_mobile.onnx | 585532 | `e47acedf663230f8863ff1ab0e64dd2d82b838fceb5957146dab185a89d6215c` |
| PP-OCRv6_rec_small.onnx | 21234383 | `6f327246b50388f3c176ae304bd95767ea6dc0c9ae92153ef8cbe210b3c14884` |
| ppocrv6_dict.txt | 74947 | `b5f2bfe2bdd9448429e3e82b51c789775d9b42f2403d082b00662eb77e401c5d` |

模型 URL 为官方注册的 `https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/`，依次接 `PP-OCRv6/det/PP-OCRv6_det_small.onnx`、`PP-OCRv4/cls/ch_ppocr_mobile_v2.0_cls_mobile.onnx`、`PP-OCRv6/rec/PP-OCRv6_rec_small.onnx`。方向模型明确是较早但配套可用的 PP-OCRv4 分类器，不冒称 v6 分类器。

字典来源为 [PaddleOCR 固定提交的原文件](https://github.com/PaddlePaddle/PaddleOCR/blob/dab3fe35379033fdcb2d0e9572fac0b36c9a9ebf/ppocr/utils/dict/ppocrv6_dict.txt)，18708 行，与识别模型嵌入 character 元数据逐行一致，仅末尾换行不同。类别精确为18710：blank、原18708符号、空格。不得改用 v4/v5 字典。

| 模型 | 真实 ONNX 输入 | 真实 ONNX 输出 |
|---|---|---|
| detector | x float32 `[-1,3,-1,-1]` | fetch_name_0 float32 `[-1,1,-1,-1]` |
| classifier | x float32 `[-1,3,-1,-1]` | save_infer_model/scale_0.tmp_1 float32 `[-1,2]` |
| recognizer | x float32 `[-1,3,48,-1]` | fetch_name_0 float32 `[-1,-1,18710]`，NTC |

预处理 profile 依据同一固定版本的 `config.yaml`：v6 检测均值/标准差均0.5，识别和方向高度48、归一化-1..1，方向顺序0/180。不同导出模型即使形状相似也需自行提供正确颜色、均值、字典和输出模式；当前验证不能推导全部 v4/v5/v6 或任意多语言模型都兼容。

## 验证状态

独立 MSVC19.51/C++17、OpenCV5.1.0、ORT1.32.0 实际编译生产5入口与现有 ONNX 算子，6组280断言全部通过，0跳过。公开模型链实际推理自生成四行中英文文本、末行180°倒置、整图12°旋转和空白输入，两种有字场景均精确还原真值。另覆盖9种位深/通道组合只读、RGBA alpha、透视/竖行裁切、Unicode及混合token五类过滤、NTC/TNC、概率/logits、canonical empty、最小f32超界、非法四边形/配对/字典、候选预算和处理若干区域后取消的原子输出。完整引擎、CLI、C ABI/SDK门禁以本阶段集成日志为准；此独立算子证据不代替它们。

文本PNG是本仓库自生成的可控真值软件夹具，使用已安装Arial Bold/SimHei的栅格输出，不分发字体文件，也不冒称现场相机照片或生产误识率验收。源图、模型和字典配套验证之外，现场字体、反光、低分辨率、吞吐与长期稳定性仍需真实业务材料另行评估。

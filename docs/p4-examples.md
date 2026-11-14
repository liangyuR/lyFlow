# P4 识别示例与合同验证

这些工作流使用正式调度器和原生 Image/Tensor，不把像素、掩膜或四边形塞入 JSON Record。需要包含 std-image 0.7.0、std-ml 0.3.0 的 LyFlow 1.3.10 运行库；桌面、CLI 和嵌入宿主共用算子实现。这里的固定夹具回归不代表现场识别率、节拍或相机验收。

## 准备与运行

在仓库根目录执行：

```powershell
python scripts/fetch-p4-model-assets.py --output-dir examples/p4-assets
python scripts/fetch-p4-model-assets.py --output-dir examples/p4-assets --verify-only
lyflow validate examples/p4-ocr.lyflow.json
lyflow run examples/p4-ocr.lyflow.json --no-cache --outputs --summary
```

已经准备过 `build/p4-assets` 时，可以离线复用经过同一清单验证的文件：

```powershell
python scripts/fetch-p4-model-assets.py --output-dir examples/p4-assets --asset-dir build/p4-assets --offline
```

清单 [p4-model-assets.json](../packaging/p4-model-assets.json) 固定十个文件的尺寸、SHA256、来源修订和许可。下载器不会用同名但校验错误的缓存文件。模型和字典由使用者准备，不随示例 JSON 重复提交。完整来源和许可见 [P4 资产说明](p4-assets.md)。

模型图参数默认使用 `p4-assets/...`，相对于工作流文件目录解析。移动或保存工作流时，要把对应 `p4-assets` 和夹具一起移动；也可显式传入模型或图片的绝对路径。标注索引里的图片和标注路径相对于索引文件目录解析。

## 工作流和预期结果

| 工作流 | 默认结果 | 隔离覆盖参数 | 覆盖结果 |
| --- | --- | --- | --- |
| [p4-annotations-native](../examples/p4-annotations-native.lyflow.json) | 三个实例，两个真实掩膜，一个无掩膜候选 | `pickableOnly=true`、`includeOccluded=false` | 只保留 `candidate`，无掩膜状态保持 |
| [p4-annotations-legacy](../examples/p4-annotations-legacy.lyflow.json) | 显式 `xyz-samples-v2` 格式，同样三个实例 | 同上 | 只保留原始 ID `annotation-2` |
| [p4-segmentation](../examples/p4-segmentation.lyflow.json) | PP-HumanSeg 在公开宇航员图片上输出非空人体分割 | `minArea=1048576` | 没有满足面积条件的实例，成功返回空集合 |
| [p4-segmentation-retained](../examples/p4-segmentation-retained.lyflow.json) | 保留 `plate-A`、`plate-B` 掩膜，只推理缺掩膜候选 | `replaceExistingMask=true` | 不保留已有掩膜，对三个 ROI 全部推理 |
| [p4-ocr](../examples/p4-ocr.lyflow.json) | 检测并识别四行中英文，末行方向纠正为 180° | `characterSet="digits"` | 合并文字为 `12345\n12345\n2026`，保留每行 `rawText` |
| [p4-barcodes](../examples/p4-barcodes.lyflow.json) | Code39 `AAAA` 与二维码 `LyFlow-P4` | `formats="QRCodeModel2"` | 只返回二维码 |
| [p4-qr-refine](../examples/p4-qr-refine.lyflow.json) | 原码、重建码均为 `LyFlow-P4`，输出 232×232 | `modulePixels=4` | 输出 116×116，内容不变 |

例如：

```powershell
lyflow run examples/p4-ocr.lyflow.json --param 'characterSet="digits"' --outputs --summary
lyflow run examples/p4-segmentation-retained.lyflow.json --param replaceExistingMask=true --outputs --summary
lyflow run examples/p4-barcodes.lyflow.json --param 'formats="QRCodeModel2"' --outputs --summary
lyflow run examples/p4-qr-refine.lyflow.json --param modulePixels=4 --outputs --summary
```

图参数覆盖只影响当前运行；再次不带参数运行得到默认结果。不要用 `--set` 覆盖已经被图参数绑定的节点参数。

### 分割和标注

分割链为 `image.segmentation_preprocess → ml.onnx_run → image.segmentation_decode`，模型契约固定为 `pphumanseg_2023mar`，单 float32 NCHW 概率头，输入 3×192×192。`batchMode=serial` 对每个实际需要推理的 ROI 单独运行 batch=1 模型。

保留示例把标注的 `instances/masks/maskInfo` 成组接到预处理的 `rois/roiMasks/roiMaskInfo`。`replaceExistingMask=false` 时逐 ROI 判断：两个非零原生掩膜保持原 ID 和像素支持，缺掩膜的第三个 ROI 才进入模型。因此 `quality.retainedCount=2`、`quality.roiCount=1`；切换为 `true` 后分别为 0 和 3。第三个 ROI 是小型合成工件，不承诺人体模型一定识别出物体；“已推理”和“有检测结果”由不同字段表达。

输出 `masks` 是 float32 `[N,H,W]` 二值 Tensor，`maskInfo.items[index]` 以 ID 和层索引对应 `instances`。重叠实例保持独立层；`labels` 为 u16 单标签图，不能替代多实例掩膜。已有标注类别不一定出现在人体模型的类别表中，`retainedUnmappedClasses` 记录此情况，未映射类别不被冒认成人体。标注源的 `pickable` 只用于筛选，输出实例始终 `pickable=false`。

### OCR

完整链包含八个处理节点和一个图片源：检测预处理、DB 模型、DB 解码、透视裁切、方向模型、方向校正、识别模型、CTC 解码。三个模型分别固定 `ocr-db`、`ocr-direction-2`、`ocr-ctc` 契约。方向和识别使用串行 batch=1，检测使用单图推理；不接受未声明的额外输出头。

`quads` 为原图像素坐标 `[N,4,2]` Tensor，顺序 TL/TR/BR/BL。`regions`、`instances`、`layout`、`results` 通过稳定 ID 和几何摘要关联，裁切后的像素不伪装成原图坐标。字典使用清单固定的 PP-OCRv6 字典，blank=0，最后追加空格类；不可混用其他模型字典。

自有图片真值：

```text
LYFLOW 12345
INDUSTRIAL OCR
工业视觉 12345
HELLO 2026
```

模型可能在拉丁字母与数字间增加空格；合同按上述内容去除 ASCII 空格后核对，保留原始返回文字供审计。末行夹具本身旋转 180°，结果必须反映实际方向校正。数字过滤后第二行为空且 `accepted=false`，该条目和 `rawText` 仍保留。无文字时成功返回空文字和空实例，不制造识别内容。预处理、透视裁切和解码采用全分辨率坐标契约，直接传入缩小图会得到明确诊断。

### 条码和二维码重建

多符号示例使用自有 Code39 与标准版本 1、L 纠错级二维码。`detections` 保存解码有效性、文本、原始字节和实际角点，`instances` 使用相同 ID。空文本与解码失败分别处理；合法空内容码的行为由原生永久回归覆盖。

二维码重建显式指定 `moduleCount=21`，不会从错误版本猜测模块数。输入使用原图检测四边形，输出带完整四模块静区。输出像素属于 `qr_refined` 帧，第二次解码也使用此帧；`feedback.outputToSource` 将重建像素映射回源图。

该干净夹具原本就能解码，重建前后均成功，`improved=false`。反馈记录相同整流、相同静区条件下的前后解码结果，不能将输出尺寸变化描述为识别改善。

## 夹具来源与再生

`p4-ocr-scene.png` 是本仓库生成的文字图，不是真实照片。`p4-ocr-scene-truth.json` 记录文字、旋转、字体来源及 SHA256。生成脚本只使用本机安装且有权使用的字体渲染，不分发字体文件：

```powershell
python scripts/generate-p4-ocr-fixture.py
```

`p4-barcodes.png`、`p4-qr-source.png` 为本仓库生成图形，无外部图片或字体。`p4-barcode-truth.json` 记录 payload、二维码完整 21×21 模块真值、尺寸和文件 SHA256。再生需要 Python NumPy/OpenCV，脚本通过 OpenCV QR 编码器生成标准码，并按 Code39 宽窄条规则绘制一维码：

```powershell
python scripts/generate-p4-barcode-fixtures.py
```

PNG 压缩字节可能随编码器版本变化，重新生成时同步记录其 SHA256，解码内容与模块几何真值保持受检。宇航员图片为清单标明来源的 NASA 公共领域图片。标注示例为自有合成多边形、洞及重叠掩膜，使用带中文和逗号的文件名验证路径处理。

## 正式入口验证

结构校验可以单独执行，不需要模型或运行库：

```powershell
python tests/release/p4_contract.py --schema-only
```

完整 CLI 合同验证要求已构建的 P4 CLI 和全部十个校验通过的资产，不会自动构建或跳过缺失模型：

```powershell
python tests/release/p4_contract.py --cli build/rust/debug/lyflow.exe --assets build/p4-assets
```

脚本把工作流及相关资产保存到新的 Unicode 目录，逐图执行“默认→覆盖→默认”共 21 次，校验 JSON Schema、正式成功状态、上述业务真值、原生张量元数据和 ID 绑定，验证结果恢复且图文件、模型和源图 SHA256 不变。日志及 `report.json` 默认保存在 `build/p4-cli-contract`；`--examples` 和 `--output-dir` 可指定安装包示例与输出目录。

`core/tests/integration/p4_recognition.cpp` 的七组集成测试经直接调度器、C ABI 和动态 C++ SDK 的保存重载入口比较完整原生图像、掩膜和四边形数据。上述 CLI 脚本通过现有 Rust 客户端执行工作流并验证 JSON 输出元数据，未专项验证 Rust SDK 的完整原生数组读取。CLI JSON 只包含大数据元信息，因此 JSON 合同测试不能替代这组原生载荷比较。SDK 安装验证、真实公开模型测试和原生错误/取消回归共同作为交付检查，具体门禁见 [测试说明](testing.md)。

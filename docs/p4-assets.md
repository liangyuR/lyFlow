# P4 示例模型与图片

这些资产用于显式运行 OCR 和人像语义分割示例，不随运行库 ZIP 隐式安装，也不由算子后台下载。下载和推理都在用户选定的本机路径进行。准备器仅使用 Python 3.11+ 标准库；运行 ONNX 模型使用 LyFlow 已交付的 ONNX Runtime，不需要 Paddle 或 RapidOCR 的 Python 包。

## 准备和离线复用

在已恢复 SDK 的根目录运行：

```powershell
python scripts/fetch-p4-model-assets.py --output-dir build/p4-assets
python scripts/fetch-p4-model-assets.py --output-dir build/p4-assets --verify-only --offline
```

SDK 中保留相同的 `scripts`、`packaging`、`examples` 相对目录；也可通过 `--config` 指定清单、`--output-dir` 指定资产目录。默认输出位于脚本所在根目录的 `build/p4-assets`。复制到其他位置时请同时保留 `examples/p4-ocr-scene.png`，本地资产相对脚本根目录解析。

已有下载文件可在无网络环境复用，`--asset-dir` 可重复：

```powershell
python scripts/fetch-p4-model-assets.py --output-dir build/p4-assets --asset-dir D:/verified-p4-cache --offline
```

Linux 使用相同 Python 命令和 POSIX 路径。缓存目录优先使用下表最终文件名，因此已准备目录可以直接作为下次的 `--asset-dir`。许可证也可从既有缓存名 `paddle-license.txt`、`rapid-license.txt`、`LICENSE` 复用，准备器通过清单 `reuseName` 映射为明确的最终名称；同一目录的两个名称都会检查，存在坏候选即拒绝，不借另一个正确文件绕过。每次复用及 `--verify-only` 都重新校验字节数和 SHA-256。缺失文件会明确失败；已有坏文件不会覆盖、修复或绕过为一次网络下载，需用户另行检查并选择干净目录。

准备器拒绝链接、Windows reparse 路径及其祖先、跨平台不安全文件名、目录穿越、重复文件名、HTTP/带凭据URL及HTTPS降级重定向。单资产上限64MiB、总量512MiB、清单256KiB。下载和复制经过有界临时文件与完整哈希校验后，以同目录硬链接原子发布，目标存在时不覆盖；文件系统须支持硬链接。后续资产失败不会回滚已经完整校验的文件；中间 `.part` 会清理，不把半份文件作为可用模型。

## 固定资产与来源

清单 [p4-model-assets.json](https://github.com/liangyuR/lyFlow/blob/v2.0.3/packaging/p4-model-assets.json) 是字节数、SHA-256、固定URL、模型正式名称和许可证对应关系的唯一来源。共10项，模型均为可选示例资产：

| 最终文件名                                 | 用途与上游版本                                   | 来源与许可                                            |
| ------------------------------------------ | ------------------------------------------------ | ----------------------------------------------------- |
| det.onnx                                   | PP-OCRv6_det_small，DB检测头                     | RapidOCR v3.10.0模型注册表，PaddleOCR权重，Apache-2.0 |
| rec.onnx                                   | PP-OCRv6_rec_small，CTC识别头                    | 同上，Apache-2.0                                      |
| cls.onnx                                   | ch_ppocr_mobile_v2.0_cls_mobile，0°/180°方向分类 | RapidOCR v3.10.0的PP-OCRv4分类注册项，Apache-2.0      |
| paddle-v6-dict.txt                         | 与v6识别模型成对的字符字典                       | PaddleOCR固定修订文件，Apache-2.0                     |
| PaddleOCR-LICENSE.txt                      | 权重及字典的上游许可证全文                       | PaddlePaddle/PaddleOCR固定修订                        |
| RapidOCR-LICENSE.txt                       | 模型转换、分发与注册来源的许可证全文             | RapidAI/RapidOCR固定修订                              |
| human_segmentation_pphumanseg_2023mar.onnx | PP-HumanSeg，两类语义分割                        | OpenCV Zoo固定修订，模型目录Apache-2.0                |
| pphumanseg-LICENSE.txt                     | 分割模型目录许可证全文                           | OpenCV Zoo固定修订                                    |
| astronaut.png                              | 512×512 RGB照片，Eileen Collins                  | NASA照片，scikit-image归档；公共领域                  |
| ocr-scene.png                              | 1200×800 RGB中英文及180°文字夹具                 | 本仓库生成，原PNG随examples交付                       |

OCR注册表固定 [RapidOCR v3.10.0](https://github.com/RapidAI/RapidOCR/tree/bd941dae09da793e76c6ba15c7aaf40f8424232a)，修订 `bd941dae09da793e76c6ba15c7aaf40f8424232a`。模型从该注册表指向的 ModelScope v3.10.0 地址获取，并固定注册表 SHA-256。即使远端标签或文件发生变化，哈希不匹配也会拒绝使用，不自动升级模型。字典和许可证固定 [PaddleOCR修订](https://github.com/PaddlePaddle/PaddleOCR/tree/dab3fe35379033fdcb2d0e9572fac0b36c9a9ebf) `dab3fe35379033fdcb2d0e9572fac0b36c9a9ebf`；字典为74,947字节，SHA-256 `b5f2bfe2bdd9448429e3e82b51c789775d9b42f2403d082b00662eb77e401c5d`。

分割模型固定 [OpenCV Zoo修订](https://github.com/opencv/opencv_zoo/tree/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/human_segmentation_pphumanseg) `48b10fa505aeb51fbd127fc60a5c660040e823b7`。照片字节固定 [scikit-image修订](https://github.com/scikit-image/scikit-image/blob/e8a42ba85aaf5fd9322ef9ca51bc21063b22fcae/skimage/data/astronaut.png) `e8a42ba85aaf5fd9322ef9ca51bc21063b22fcae`，而非浮动的在线示例图。

## 许可与图片生成

核对日期为2026-11-15。[RapidOCR官方模型许可声明](https://rapidai.github.io/RapidOCRDocs/main/en/model_licenses/) 将其注册的PaddleOCR派生模型列为Apache-2.0，并区分权重、分发代码、字典和字体的权利归属。这里字典另从固定PaddleOCR源码与其许可证取得，不把模型许可声明延伸到所有外部资源。声明HTML会变化，因此只作为出处链接，不作为准备器可下载的许可证全文；全文来自清单固定修订的LICENSE文件。再分发模型时需保留这些许可证与来源说明。

[scikit-image官方astronaut文档](https://scikit-image.org/docs/stable/api/skimage.data.html#skimage.data.astronaut) 说明其来自NASA Great Images，并记录为无已知版权限制、公共领域。不把这一张照片的许可推广到整个数据集，也不暗示NASA认可本产品。

`examples/p4-ocr-scene.png` 是独立生成的文字栅格图；真值、源字体说明与固定PNG哈希在 [p4-ocr-scene-truth.json](../examples/p4-ocr-scene-truth.json)。原始夹具使用本机已安装的 Arial Bold 与 SimHei，只交付栅格结果，不分发字体文件。重新生成需要 Pillow 与自行合法取得的字体：

```powershell
python build/native-sdk/scripts/generate-p4-ocr-fixture.py --output build/regenerated/p4-ocr-scene.png
```

非Windows环境可通过 `--latin-font`、`--chinese-font` 指定本机字体。更换字体或Pillow版本可能改变栅格字节及识别结果；该脚本生成新的真值文件，不会修改固定资产清单。不能把不同栅格静默替换成已验证PNG。

## 验证边界

资产准备器的永久测试涵盖离线/本地/网络模拟、逐次校验、坏缓存、固定大小/哈希、重定向、跨平台路径、链接祖先、竞争写入、临时文件清理及清单中本地PNG与真值的对应。真实 `build/p4-assets` 已用既有固定缓存完成10项离线准备并重新逐项验证。准备成功只证明字节与清单一致，不证明模型精度、全部语种、现场反光/曲面、生产吞吐或长期稳定性。

模型接口、预处理、坐标和真实有限样本结果分别见 [OCR分析](p4-analysis-ocr.md) 与 [分割分析](p4-analysis-segmentation.md)。模型路径和字典路径由工作流显式配置；不向运行库ZIP塞入可选权重，不把字符空结果当成下载或推理错误。

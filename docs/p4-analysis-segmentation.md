# P4：语义分割、ROI 回贴与原生实例支持

本项独立实现 `image.segmentation_preprocess → ml.onnx_run → image.segmentation_decode`。像素载荷使用原生Image/Tensor；Record只保存有界的参考帧、类别、ROI映射、实例ID和质量说明。实现见 [p4_segmentation.cpp](https://github.com/liangyuR/lyFlow/blob/v2.0.3/packs/std-image/ops/p4_segmentation.cpp)，永久回归包括[合成边界](https://github.com/liangyuR/lyFlow/blob/v2.0.3/packs/std-image/tests/test_p4_segmentation.cpp)与[真实模型链](https://github.com/liangyuR/lyFlow/blob/v2.0.3/packs/std-image/tests/test_p4_segmentation_models.cpp)。

## 外部参考的可知与不可知范围

已逐个阅读本机 `xyz-vision-lib` 的 `Learning/SemanticSegmentor.cpp/.hpp`，以及 `property_settings.yml` 的默认值、英文参数元数据和中文说明。该目录没有 `.git` 元数据，不能确认它等于清单中的 `72d…` 提交。只分析接口、参数与业务边界，没有复制 proprietary 函数体。

| 本机参考文件 | SHA256 |
| --- | --- |
| SemanticSegmentor.cpp | A2F90D91C48788C880F56224C3D9493F83247AA13025266FF5C70E25A7F32A35 |
| SemanticSegmentor.hpp | BA7F4982F0945113499CD45A1D5084154CC3042E77379219656B7EDF82846665 |
| property_settings.yml | 9CF69C5762A2BC2192A9EEBEC59A52DFE77F8BA466556AD8E3CF7D7ACDCC71F1 |

外部输入为 Color 和可选 Primitives2D ROI，输出为携带 mask 的二维实例及可视化图。封装指定 onnxruntime/SemanticSegmenter，再调用不可得的 `xlb2::SemanticSegmenter`。底层预处理、模型 ZIP 内容、类别协议、批次/重叠规则及可视化细节未提供，不能声称已阅读或兼容。

| 外部语义/参数 | 实际静态行为 | 本项明确规则 |
| --- | --- | --- |
| 未连 ROI / 连空列表 | 未连时传空 ROI 给底层；已连接但列表为空时不调用模型。中文描述与已连空列表分支不一致。 | 未连为整图；已连空列表保留空批次，不退回整图。 |
| replace_existing_mask=false | 遇任一已有 mask 时整批直接透传，其他 ROI 也不再推理，可视化置空。 | 默认替换；replaceExistingMask=false逐ROI保留合法原生支持，只推理缺mask的ROI。全保留时零次Run，部分保留仍计算其余ROI，不继承整批短路缺陷。 |
| ROI 结果绑定 | 底层结果按 ROI 顺序展开；封装将 ROI 的原 score/box 覆盖到各输出实例。 | 实例框、面积及平均置信度从实际原图掩膜计算，保存 roiId，不继承未经分割验证的框或分数。 |
| max_batch_size=8 | 底层实现不可得。 | 显式 `batchMode=serial`；静态 batch=1 模型逐 ROI 真正 Run 并拼接首维，最多64 ROI。 |
| bbox_dilation_ratio=1.1 | 仅转交给底层。 | `bboxDilation` 默认1.1，中心膨胀后裁到图内；整数裁剪采用左上 floor、右下 ceil 的半开区域。仅支持 angle=0。 |
| mask_threshold=0.5 | 底层阈值实现不可得。 | `maskThreshold` 作用于回贴后的最大类别概率；低于阈值为 unknown。 |
| executor/device/TRT/FP16 | 参数包含 cpu/cuda/tensorrt、设备、线程及缓存设置。 | 本轮使用官方 ORT CPU float32；GPU、FP16、TRT 引擎及外部 ZIP 协议不在支持范围。 |
| 文字/框/箭头/MD5/耗时可视化 | 参数与底层 Visualize 相连。 | 输出 RGB(A) 掩膜色彩叠加；Unicode 类名保留在结果 Record，不用不支持 Unicode 的字体伪造文字叠加。 |

## 固定公共模型与照片

选用 [OpenCV Zoo PPHumanSeg 目录](https://github.com/opencv/opencv_zoo/tree/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/human_segmentation_pphumanseg) 中的 FP32 `human_segmentation_pphumanseg_2023mar.onnx`。目录 README 明确所有文件采用 [Apache 2.0](https://github.com/opencv/opencv_zoo/blob/48b10fa505aeb51fbd127fc60a5c660040e823b7/models/human_segmentation_pphumanseg/LICENSE)，其中保留 PaddlePaddle Authors 版权声明。发布模型时须一并携带完整 LICENSE；代码仓库许可不能替代模型许可。

| 资产 | 固定来源/规格 | SHA256 |
| --- | --- | --- |
| FP32 ONNX | Zoo commit `48b10fa505aeb51fbd127fc60a5c660040e823b7`；6,163,938 B | 552d8a984054e59b5d773d24b9b12022b22046ceb2bbc4c9aaeaceb36a9ddf24 |
| 模型 LICENSE | 同提交模型目录；11,437 B | d9cf3b547fa4232fb9285f2db52e909f10f00781aaf64a6d0bb90c55149f1363 |
| astronaut.png | [scikit-image v0.25.2 固定提交](https://github.com/scikit-image/scikit-image/blob/e8a42ba85aaf5fd9322ef9ca51bc21063b22fcae/skimage/data/astronaut.png)；512×512 RGB；791,555 B | 88431cd9653ccd539741b555fb0a46b61558b301d4110412b5bc28b5e3ea6cb5 |

照片为 NASA 的 Eileen Collins 肖像；[scikit-image 官方数据说明](https://scikit-image.org/docs/0.25.x/api/skimage.data.html#skimage.data.astronaut) 标明来自 NASA Great Images，公有领域、无已知版权限制。它只用于软件推理验证，不表示人物或机构背书。没有使用已叠加的模型示例输出充当原始输入。

已读取固定 ONNX 的真实元数据与末端算子，并用 ORT1.32 CPU 实际执行：

- 恰好一个输入 `x`：float32 `[1,3,192,192]`，RGB NCHW，逐通道 `pixel/127.5-1`。
- 恰好一个输出 `save_infer_model/scale_0.tmp_1`：float32 `[1,2,192,192]`，类别0背景、类别1人员。末端已有 Softmax，输出为概率。
- 模型没有动态 batch。运行图选择 `modelContract=pphumanseg_2023mar` 和 `batchMode=serial`，每个 ROI 单独执行静态 batch=1 模型。多头、其他 dtype、错误名称或形状明确拒绝，不能只拿第0头。
- 空批次仍校验模型契约，执行零次 Run，输出规范空 Tensor `{shape:[0], data:[]}`；解码仅在映射 ROI 数也为0时接受此形式。

## 端口、参数与坐标

| 节点 | 输入 | 输出 |
| --- | --- | --- |
| image.segmentation_preprocess | image：u8 灰度/RGB/RGBA；可选 rois：同帧px的Poses2D；可选roiMasks+roiMaskInfo与rois形成原生三元组 | tensor：float32 `[R,3,Hmodel,Wmodel]`；mapping；retainedInstances、retainedMasks、retainedMaskInfo原生保留三端口 |
| ml.onnx_run | input：上述 Tensor；modelPath 固定公共模型 | output：float32 `[R,2,192,192]`，空批次为 `[0]` |
| image.segmentation_decode | 原始image、mapping、output；有保留项时同时连接pre的retained三端口 | labels：全图单通道u16 Image；masks：float32 `[N,Himage,Wimage]`；maskInfo、instances、quality小Record；overlay：RGB/RGBA Image |

图连接为 `pre.tensor → infer.input`、`infer.output → decode.output`、`pre.mapping → decode.mapping`；同一原始图同时接 pre.image 与 decode.image。文件读取只在 I/O 节点执行 BGR→RGB，分割节点不再次交换通道。

保留模式还把 `pre.retainedInstances/retainedMasks/retainedMaskInfo` 分别接到decode同名端口。上游标注加载可把 `annotation.image/instances/masks/maskInfo` 分别接到pre的 `image/rois/roiMasks/roiMaskInfo`，无需把掩膜转成JSON。

预处理默认 `modelContract=pphumanseg_2023mar`，强制192×192和2类。`classNames` 可以提供两个不同的 Unicode 名称，但索引顺序仍必须对应模型输出；不靠标签文字改变模型能力。`frame` 默认 image；`bboxDilation` 允许1..4。灰度复制为RGB，RGBA忽略 alpha 参与推理，源数据始终只读。

另提供显式 `semantic_nchw` 契约：仍只做RGB `[-1,1]`，不自动推断其他归一化或NHWC。inputWidth/inputHeight为1..2048；输出空间尺寸须等于声明的输入尺寸，不自动猜测低分辨率分割头的比例。多类需要2..256个唯一名称。选择headKind=binary时接受单通道 `[R,1,Hmodel,Wmodel]`，名称数组仍有两个元素，binaryForegroundClass明确该通道对应哪个索引。概率直接映射；logit用稳定sigmoid，另一类概率为1-p。多类logits采用减最大值的稳定softmax。通用模型的名称、尺寸、归一化及语义由调用者明确声明；它不代表任意ONNX格式已经兼容。

解码默认 outputKind=probabilities、maskThreshold=.5、backgroundClass=0、minArea=1、maxInstances=256、overlayAlpha=.5、idPrefix=semantic。PP 契约禁止 outputKind=logits，防止重复 softmax。多类概率必须有限、位于0..1且每个像素类别和与1相差不超过1e-3；单通道二值概率不要求其单通道值等于1。backgroundClass=-1表示所有类别均可产生实例；二值前景索引不能同时被声明为背景。

仅接受 `image.scale=1`。本轮不把低分辨率预览结果冒充原图分割；两个算子的 previewable=false。ROI几何为原图像素坐标，角度非零明确拒绝；部分越界裁剪、完全越界保留skippedRoiIds且不调用模型。映射严格校验模式、形状、类别、唯一ROI ID、裁剪边界及源图内容指纹，检测同尺寸图像错配。内容指纹用于载荷配对检查，不是产品指纹、来源认证或防篡改身份保证。

概率先双线性放回实际裁剪区域，然后 argmax；同概率选择较小类别索引。低于阈值及所有 ROI 外像素为 **65535 unknown**，确定背景为类别0，两者语义不同。重叠 ROI 的类别图按较高最大类别概率选择，相等时保持先前 ROI 的结果。

每个 ROI 各自按非背景类别的8连通分量形成实例，过滤小于 minArea 的分量。输出框是实际原图支持的轴对齐包围框，confidence 是该支持的平均类别概率；不继承输入 ROI 的 confidence。聚合类别图保留 minArea 过滤前的分类像素；minArea 只过滤实例，quality 明确记录此阈值。相互接触的同类目标可能连为一个分量，不能将该输出称为学习式实例分割或目标数量真值。

## 逐ROI保留和替换已有掩膜

`replaceExistingMask=true`为默认，合法原生三元组仍会完整校验，但全部有效ROI重新推理。false时，pre逐项检查roiMaskInfo的hasMask/source与真实NHW层，不凭Pose.attributes.hasMask决定跳过：

- roiMasks和roiMaskInfo必须同时存在且与rois同帧、同extent、同序唯一ID及index绑定；Tensor形状必须严格为 `[rois.items.size(),Himage,Wimage]`，值只能为0/1。没有原生两端口时，Pose内的hasMask标记不会跳过推理。
- hasMask=true必须有非零原生支持，且所有支持落在该ROI原始框裁到图内的有效像素范围；不利用膨胀参数掩盖错绑或越界支持。声明true却全零、false却有非零值、hasMask/source或可选attributes.maskIndex矛盾都拒绝。
- hasMask=false且原生层为空的ROI继续进入推理；合法true项从推理批次剔除，原ID、class、confidence、box保留，pickable强制false。不会保留一个ROI后停止整批计算。
- pre总会输出保留三端口，没有保留时为同帧空集合和 `[0,H,W]`；mapping记录retainedIds及内容配对指纹。decode要求保留三端口成组、形状/有序ID/帧/数据有效且与指纹一致，拒绝混搭另一批的掩膜或元数据。
- decode先按原输入顺序交付保留实例，再追加新分割实例，同步所有mask index和ID。新生成ID与保留ID碰撞时原子拒绝，调用者可用idPrefix区分命名空间，不静默改写原ID。

保留对象的原类别不能冒充当前模型的类别。仅当其classId为模型范围内规范十进制索引，且存在classLabel时精确等于对应classNames，类别Image才写入该已知类别。`classId="1"/classLabel="连接件"`不等于PP的person：保留原Pose与独立mask，但类别图相应支持为unknown，quality的retainedUnmappedClasses加一。未提供或无法对应的类别也如此处理。叠加仍显示保留支持，不虚构新类别。

保留支持在聚合类别图中优先于重新推理的重叠支持；保留项之间按原序选择单张类别图的值。所有独立NHW层仍保留重叠。已保留对象仅将其真实支持回贴到类别图，不把其框内剩余像素推断为背景。保留项叠加使用稳定实例色，重新推理区域使用类别色，RGBA alpha始终保持。

## 原生 mask 与实例绑定

`masks` 是float32 NHW二值 Tensor，值严格0/1；N是最终连通实例数，H/W是完整原始图像尺寸。每个实例独立一层，所以重叠 ROI 的实例支持仍能重叠。空结果的 shape 为 `[0,H,W]`、data为空。超过预算原子报错，不静默丢弃尾部实例。

`maskInfo` 的类型为 `vision.Masks2D.v1`，仅包含：

```json
{"frame":"image","unit":"px","width":512,"height":512,"layout":"NHW","items":[{"id":"semantic-0-1-0","index":0,"hasMask":true,"source":"semantic_connected_component"}]}
```

items 与 `vision.Poses2D.v1.items` 同序同ID，index 对应原生 Tensor 第0维。Tensor 没有独立资产ID，这是一组显式成对端口契约；下游必须一起携带并验证形状/有序ID，不能把其他节点的 Tensor 与该 Record 混搭。idPrefix 可用于并行模型命名空间，改前缀会同步修改 maskInfo 与 Pose ID。

新分割Pose的attributes包含roiId、maskIndex、pixelCount、hasMask=true、source=semantic_connected_component；classId是类别索引字符串，classLabel保留Unicode。保留项维持原类别及业务属性，规范maskIndex/hasMask与新顺序一致。所有实例均 `pickable=false`。语义mask、连通分量、置信度与二维框都不构成可抓许可；不产生机器人位姿或生产安全判定。

新推理区域的overlay仅混合确定的非背景类别，不改变unknown/背景像素；保留支持按上节规则显示，类别不可映射时仍显示独立实例色。灰度转为RGB，RGB保留三通道，RGBA逐像素保留原alpha。此输出不修改源Image、模型输出Tensor或ROI Record。

## 资源、取消与验证证据

硬边界为图像8Mi像素、64 ROI、输入/输出及展开概率16Mi float元素、ROI面积×类别128Mi次检查、实例mask32Mi float元素、最多256实例。另对图像、概率展开、ROI临时矩阵、队列和mask向量增长作保守512MiB预估；预算不允许时返回 resource_limit。多个大尺寸全图实例会因NHW成本被明确拒绝，不能用压成单张标签图来丢失重叠。

像素循环、类别循环、连通遍历与散射均轮询取消；OpenCV单次 resize/分配在有界阶段内完成。取消及验证/预算错误均在任何端口发布前返回；ONNX serial 层也在批次边界取消，不交付部分批次。

独立 Windows MSVC19.51/C++17/MD/RelWithDebInfo、OpenCV5.1及官方ORT1.32探针直接编译生产实现。真实照片推理链结果：

| 输入场景 | 模型实际输出 | 原生实例mask | 结果 |
| --- | --- | --- | --- |
| 512²照片整图 | `[1,2,192,192]` | `[1,512,512]` | 1个连通实例、105511前景像素 |
| 两条重叠ROI | 两次真正Run后拼接 `[2,2,192,192]` | `[2,512,512]` | 2份独立支持、聚合108697前景像素 |
| 显式连接空ROI | `[0]`，零次Run | `[0,512,512]` | 0实例、262144像素全部unknown |
| 全黑无目标图 | `[1,2,192,192]` | `[0,512,512]` | 0前景、0实例；确定背景不冒充unknown |

RGBA alpha=37在模型链与叠加后保持；实际叠加图已目视核验。记录保留在 ignored `build/p4-segmentation-review/real-results.json`、`tests-real.log` 和 overlay-0/1.png。独立真实模型验证与永久合成边界测试分开记录；这些有限软件样本不证明生产分割精度、人员安全检测、吞吐或长期稳定性，完整C ABI/Client SDK与发布组合由主线门禁另验。

另以真实模型产出的既有支持复验保留/替换组合：全保留、部分保留、全部替换分别产生0/1/2项推理Tensor，生产serial执行路径在每次Run完成后上报的progress计数分别严格为0/1/2；模型输出分别为 `[0]`、`[1,2,192,192]`、`[2,2,192,192]`。全保留时原mask逐位相同，部分保留时原支持不变并追加另一层，替换时retainedCount=0。记录为 `retained-real-results.json` 和 `tests-retained-real.log`。永久回归另覆盖错绑/空支持/越ROI、数字相同但名称冲突、载荷指纹错配、保留ID碰撞及alpha保持；不是用预制Tensor声称实际Run次数。

两组真实模型永久用例由 `LYFLOW_P4_MODEL_TESTS` 构建开关启用，运行要求环境变量 `LYFLOW_P4_ASSETS` 指向包含上述固定ONNX和astronaut.png的目录。缺资产直接失败，不将缺模型记为成功。测试直接使用生产ensureRegistry/OpCall与ONNX compute，断言合理有效支持范围和同一运行库内的保留掩膜一致性，不把某个CPU上的105511像素硬编码成跨平台精度真值。

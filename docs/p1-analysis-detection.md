# P1 固定模型检测链分析

[补齐计划](operator-completion-plan.md#b05) · [能力对照](operator-coverage.md)

本机参考源码没有 Git 元数据；以下核对当前文件，不能声称等同清单的指定提交。外部源码为 proprietary，仅分析能力、问题与契约，LyFlow 独立实现。

| 外部条目 | 已阅读源码与实际行为 | 问题与 LyFlow 选择 |
|---|---|---|
| Detection2D | `models/Learning/Detection2D.cpp`、同名头文件；连接颜色快照、首个矩形 ROI 和人工辅助工件，调用不可得的 learning bridge 的模型内部实现未在参考仓库提供，不声称复现其专有推理。本轮按实际导出契约提供四个已验证的 CPU 检测配置：

| 模型 | 预处理与固定公开模型签名 | 解码 |
|---|---|---|
| YOLOX-Nano 0.1.1rc0 | BGR float32 NCHW 0..255，416²、右下补 114；`[1,3549,85]` | 步长 8/16/32 网格，objectness×最高类别概率，类别感知 IoU NMS |
| YOLOv8n | RGB float32 NCHW 0..1，640²、居中补 114；`[1,84,8400]` | 已解码 cx/cy/w/h、80 类分数，无 objectness，类别感知 IoU NMS |
| YOLO11n | 同上；`[1,84,8400]` | 同上 |
| YOLO26n | 同上；`[1,300,6]` | 端到端 xyxy/score/class，过滤与排序，避免重复 NMS |

`image.yolox_preprocess → ml.onnx_run(modelContract=yolox-nano-0.1.1rc0) → image.yolox_decode` 对接 YOLOX。`image.yolo_preprocess → ml.onnx_run(modelContract=yolo-detection) → image.yolo_decode` 对接 YOLOv8/11/26；必须同时选择匹配的 modelFamily 与模型文件。新三系列支持显式 `classNames`（1..512 类）及 32 的倍数、32..2048 的静态输入尺寸；`outputFormat=auto` 按系列选择常规或端到端，显式 raw/end2end 可对接对应导出格式。常规输出仅接受 `[1,4+C,N]`，N 为 8/16/32 步长的总锚点数；端到端接受 `[1,N,6]`，1≤N≤4096。模型必须是单输入/输出、静态 float32；动态、FP16、转置检测头、分割和 OBB 需单独适配，不能只换文件假定兼容。

输出为 `vision.Poses2D.v1`；中心/尺寸为原图像素、角度 0、类别 ID 为字符串、`pickable=false`。真实抓取可用性不能从检测置信度推断。预处理支持 u8 灰度/RGB/RGBA，u16/f32 必须先显式转换；非空 Box2D ROI 向外整数取整且位于图内。映射元数据明确实际 resize 尺寸和 padding，每轴逆映射避免整数取整误差。原图范围遵循 Image.scale 的名义像素范围。

消费者验证所有概率/坐标为有限值、声明的形状、模型标识、元数据维度与实际 letterbox 规则；非法输出报告诊断，无目标输出有效空列表。NMS 的 IoU 使用连续矩形边界，不套用像素格的 `+1` 面积。分数含阈值等号，IoU 严格大于阈值才抑制。候选与结果均有显式上限；输入只读，预处理与后处理支持取消。

后续边界：原生实例分割、可抓区域、旋转检测框、多 ROI 批处理、人工工位标注与原 Workspace/Request 协议不在这个固定检测模型切片内，因此两个外部条目继续标为部分覆盖。

## 固定资产与验证入口

资产版本、来源、长度和许可说明在 [p1-model-assets.json](../packaging/p1-model-assets.json)，通过 [下载脚本](../scripts/fetch-p1-model-assets.py) 准备。脚本不覆盖已有不同内容的文件，下载失败清理本次临时文件。运行库包沿用不包含客户/测试 ONNX 模型的发布规则，模型作为明确版本的外部资产管理；SDK 示例提供准备与运行说明。

- 预处理/后处理的确定性正反例：`packs/std-image/tests/test_detection_p1.cpp`。
- 四种真实模型、公开两图、空背景与三种执行入口：`core/tests/integration/p1_detection.cpp`，显式启用 `LYFLOW_P1_MODEL_TESTS`，资产缺失时失败而不跳过。
- CPU 性能记录使用当前环境与样例，不用 COCO 模型的公开 AP 代替工业现场质量验收。

上游依据：[官方 ONNXRuntime 流程](https://github.com/Megvii-BaseDetection/YOLOX/blob/0.1.1rc0/demo/ONNXRuntime/README.md)、[项目许可证](https://github.com/Megvii-BaseDetection/YOLOX/blob/0.1.1rc0/LICENSE)。测试图来自 scikit-image 固定 v0.22.0 的 NASA Eileen Collins 公有领域图和 Stefan van der Walt 的 CC0 Chelsea 图，许可出处见 [skimage.data](https://scikit-image.org/docs/stable/api/skimage.data.html)。这些是软件链验证数据，不是工业验收集。

YOLOv8n/11n/26n 使用 Ultralytics assets v8.4.0 的官方权重，由 `ultralytics==8.4.21`、`torch==2.10.0`、`onnx==1.17.0` 固定导出（opset17、CPU、静态、float32、不简化）；导出记录保存实际工具版本和签名。Ultralytics 权重/导出工具按上游 AGPL-3.0 或 Enterprise 条款管理，与 YOLOX 的 Apache-2.0 分开列示；不向运行库拼入上游 Python 实现或权重。上游格式说明见 [YOLO26](https://docs.ultralytics.com/models/yolo26/) 与 [导出模式](https://docs.ultralytics.com/modes/export/)。

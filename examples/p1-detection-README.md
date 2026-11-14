# P1 检测示例

支持 YOLOv8、YOLO11、YOLO26 与固定 YOLOX-Nano，输出原图像素坐标的 `vision.Poses2D.v1`。先在仓库或 SDK 根目录准备外部资产：

```powershell
python scripts/fetch-p1-model-assets.py
python -m venv build/model-export-env
build/model-export-env/Scripts/python.exe -m pip install torch==2.10.0 torchvision==0.25.0 --index-url https://download.pytorch.org/whl/cpu
build/model-export-env/Scripts/python.exe -m pip install -r scripts/requirements-model-export.txt
build/model-export-env/Scripts/python.exe scripts/export-p1-yolo-models.py
lyflow validate examples/p1-yolo-detection.lyflow.json
lyflow run examples/p1-yolo-detection.lyflow.json
```

默认运行 YOLO26n。切换 YOLOv8n/YOLO11n 时，同时覆盖图参数 `modelFamily`（`yolov8`/`yolo11`）与 `modelFile`（相应 `.onnx`）；`imageFile` 可切换 Chelsea 猫图。具体参数选项见 `lyflow run --help`。桌面打开同一图运行，嵌入宿主通过现有图参数接口按次覆盖；无需新 ABI。

另一个 `p1-detection.lyflow.json` 使用 YOLOX-Nano 0.1.1rc0，可下载后直接运行，无需导出 Python 环境。新三系列使用 `image.yolo_preprocess → ml.onnx_run → image.yolo_decode`，YOLOX 使用专用 yolox 节点。它们的颜色、归一化、letterbox、形状和后处理不同，不能混接。

| 配置 | 模型输入 / 输出 | 解码 |
|---|---|---|
| YOLOv8n、YOLO11n | RGB/255、`[1,3,640,640]` / `[1,84,8400]` | 无 objectness、类别感知 NMS |
| YOLO26n | 同输入 / `[1,300,6]` | 端到端结果，不重复 NMS |
| YOLOX-Nano | BGR 0..255、`[1,3,416,416]` / `[1,3549,85]` | 网格解码、objectness×类别概率、NMS |

自定义训练模型可显式配置类别名称、静态尺寸与 raw/end2end 输出格式；只支持包指南声明的 float32 检测签名，动态、FP16、转置头、实例分割、OBB 与抓取可用性不在本轮范围。

资产 URL、长度和许可在 `packaging/p1-model-assets.json`。YOLOX 使用上游 Apache-2.0 许可；Ultralytics 权重和导出工具遵循上游 AGPL-3.0 或 Enterprise 条款。两张公开验证图为 NASA Eileen Collins 公有领域图与 Stefan van der Walt 的 CC0 Chelsea 猫图，取自 scikit-image v0.22.0。运行库和 SDK 不内置权重或测试图片；下载脚本默认写入 `build/p1-assets`，可指定 `--output-dir`，导出脚本对应 `--assets-dir`。

验证覆盖真实四模型的两种目标与空背景、预处理颜色/填充、阈值/NMS、原图坐标回映、执行器/C ABI/C++ SDK 一致性。公开图验证不代表工业误检漏检、计量精度、现场节拍或 GPU 验收。

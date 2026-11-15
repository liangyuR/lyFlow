# std-metrology 逐算子迁移清单

状态：实施中；当前 1 项。原251/7规划基线通过机器清单baselineReference追溯。返回[清单总览](../operator-package-audit.md)。

下列顺序是迁移/验收顺序，不是执行图调度顺序。精确端口和参数见[机器清单](../operator-package-audit.json)。

## 060 · image.calibrated_distance

- 当前：`std-metrology` / `1.1.0` / 平面标定距离量测。目标：`std-metrology`；迁移归属；**M2**。
- 源码：[packs/std-metrology/ops/register.cpp:27](../../packs/std-metrology/ops/register.cpp#L27)。
- 端口：`a:Point2D`, `b:Point2D`, `calib:Record` → `distance:Measurement`。
- 参数单位标注：参数未统一声明单位；以逐项契约及源码为准。
- 依赖：当前compute：标准C++、公共calibration::Plane解析及标量H映射，不使用OpenCV；同TU其他节点使用OpenCV。；目标：轻量几何/标定模型公共核，不链接OpenCV/PCL/ORT。
- 当前契约：输入[a:Point2D, b:Point2D, calib:Record] → 输出[distance:Measurement]。两个原图像素Point2D经image.PlaneCalib映射到平面单位，再乘metersPerUnit，输出单位m的Measurement；单位/单应无效或非有限结果诊断。
- 目标契约：两个像素点经PlaneCalib得到米制距离迁std-metrology；公共单应标量映射核无OpenCV，不靠图像包注册。
- 迁移前置：M1；std-metrology量纲与Measurement契约；公共PlaneCalib验证与单应标量映射；M1统一PlaneCalib单位及齐次矩阵验证，提供旧输入显式迁移
- 关联算子：无额外逐ID前置；公共协议见前置项
- 验收：验证m/mm标定等价、px点单位拒绝、单应奇点和米制Measurement。
- 说明：主职责为具有标定基准的物理距离，图像像素坐标是输入域；必须保留先映射再换米，不能直接用像素距离乘常数。 现有PlaneCalib有两套验证器：lyflow_cv/plane_calib.h默认mm且允许非空unit，core/calibration.h要求显式m/mm并归一化H。统一校验属于契约变更，不能在搬包时静默替换。
- M1契约更新（1.1.0）：新增显式frame参数，版本化PlaneCalib使用共享schemaVersion=1/frame/unit解析；旧非版本化分支保持兼容。；验收：v1帧缺失/不匹配拒绝；米/毫米一致；旧参数与旧标定路径不漂移。
- M2边界验收（manifest_verified）：M2公共核、轻量包归属与声明式依赖边界；原ID/版本/数据数值与错误语义保持。
- 阶段依赖边界：std-metrology -> metrology-kernel -> data-contract；C++17/std及公开core校验，无OpenCV/PCL/ORT。
- 公开接口：kernels/include/lyflow/kernel/metrology.h；core/include/lyflow/calibration.h
- 阶段验收边界：当前描述已与真实全包manifest核验；独立静态核、无重后端、既有算子与安装SDK数值回归由阶段验证证据记录，不代表M3场景算法迁移。
- M2迁包来源：`std-image` / `packs/std-image/ops/image_p1_calib.cpp:445` @ `f0c043c6341755e8f254ac499a1593df087b6d94`；原目标包和依赖规划保留。

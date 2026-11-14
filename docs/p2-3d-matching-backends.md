# P2 三维匹配后端选择

[文档中心](README.md) · [阶段交付](p2-sequential-delivery.md) · [匹配分析](p2-analysis-matching.md)

2026-11-14 根据用户确认，阶段2以 Windows CPU 为主线，验证 KISS-Matcher 全局定位与 small_gicp 精配准。FoundationPose Inference Library 单列为可选 GPU 后端。本页记录选型和待验证项，不将第三方的功能说明或论文结果写成本项目已经通过的验收。

## 两条输入与执行链

| 项目 | CPU 点云匹配 | 可选 GPU RGB-D 位姿 |
|---|---|---|
| 后端 | KISS-Matcher → small_gicp → LyFlow 原生输出质量复核 | NVIDIA FoundationPose Inference Library |
| 输入 | 米制模型点云、场景点云，可选由标定图像区域得到的原场景索引 | CAD 或参考视图、RGB、米制深度、相机内参；首次注册需要目标掩膜 |
| 目标 | 无初值全局定位，再局部精化；多个实例需要额外候选管理、重复抑制和逐实例质量验证 | 单次注册与跨帧跟踪，模型及GPU工作区常驻，多对象共享帧 |
| 平台门槛 | Windows MSVC 实际构建和完整运行回归必须通过 | 上游目前提供以 Linux NVIDIA GPU 容器为主的支持与部署流程；不宣称已有原生 Windows 成品 |
| 当前状态 | Windows 节点、多实例与三档显式参数、原 CAD 保存/重载、CLI/C ABI/C++ SDK 已在本地运行；等待本阶段 PR 的 CI 与 Codex 门禁。此前独立三角假设原型不编入默认后端 | 候选设计，不作为本轮默认依赖，不列为已验证能力 |

KISS-Matcher 使用 Faster-PFH 特征、对应关系剔除和全局位姿求解，不能以已有9初值ICP代替。small_gicp提供局部精配准，仍需要合适初值；它不能单独解决全局检索。上游面向扫描配准的示例尺度不应直接用于毫米级零件，采样、邻域、对应距离和质量阈值必须按本项目的米制输入及实际采样密度定义。

## 已核对的来源与集成边界

- [KISS-Matcher 源码](https://github.com/MIT-SPARK/KISS-Matcher/tree/8b58b118e00c7a5a22448741f8d956d638a44ae2)和[发布说明](https://github.com/MIT-SPARK/KISS-Matcher/releases)：本次核对修订 `8b58b118e00c7a5a22448741f8d956d638a44ae2`，顶层MIT许可。当前源码真实MSVC编译已复现ROBIN的变长数组、GCC原子操作问题；Windows补丁需保留原子语义、求解与并行正确性，不以移除错误对应剔除来绕过构建。未修复版本不能宣布支持Windows。
- [small_gicp](https://github.com/koide3/small_gicp/tree/fa0cfc983417c044b6036f71dfa448038bb9c820)：本次核对修订 `fa0cfc983417c044b6036f71dfa448038bb9c820`，CMake版本1.0.1，MIT许可；Eigen为必需依赖，nanoflann/Sophus随源码提供，OpenMP/TBB可选。源代码明确含MSVC选项，但最终仍以本仓MSVC编译与运行结果为准。线程数受引擎预算约束，不自行占满全部CPU。
- [FoundationPose推理库](https://github.com/nvidia-isaac/foundation-pose-inference-library/tree/89dd4cfec75f944b9fca52d9cb282725f1666c12)：本次核对修订 `89dd4cfec75f944b9fca52d9cb282725f1666c12`。C ABI覆盖创建、预热、注册、跟踪、销毁与多对象group；输入约定RGB u8、米制f32深度、u8掩膜、行主序K，输出物体到相机的SE(3)。源码Apache-2.0，模型权重、CUDA/TensorRT及可选nvdiffrast各有独立许可，不能从代码许可推定全部资产同许可。来源见[C ABI](https://github.com/nvidia-isaac/foundation-pose-inference-library/blob/89dd4cfec75f944b9fca52d9cb282725f1666c12/include/foundation_pose_nvidia/c_api.h)、[第三方声明](https://github.com/nvidia-isaac/foundation-pose-inference-library/blob/89dd4cfec75f944b9fca52d9cb282725f1666c12/THIRD_PARTY_NOTICES.md)。

CPU后端输出统一使用现有Transform/Pose3D及原生点云、原场景Indices/Groups。模型居中、原模型到局部模型的变换、source→target方向和单位必须明确；最终质量针对交付float32变换重算，避免内部double矩阵合格而交付值越界。模板、ROI、边缘、可见面采样与后端选择分开，不将大型点数组放进Record。

依赖审计发现：ROBIN常规构建还链接[PMC的GPL-3.0-or-later代码](https://github.com/MIT-SPARK/pmc/blob/4bbd40ababd8e925c4e1845c509173afe766b443/LICENSE.md)，不能从KISS顶层MIT许可推定整条构建链同许可。拟交付构建只保留KISS默认的`max_core`路径：ROBIN兼容图、PKC k-core分解及KISS GNC求解均保留；排除可选`max_clique`/PMC的源码、链接和交付，未启用路径明确报告不支持。独立Windows probe已按此来源编译/链接并完成组合配准；正式构建仍需复核链接输入和全部运行回归。不能仅在运行参数中不用PMC就声称已移除依赖。LyFlow许可不作变更。

GPU候选以独立常驻执行资源适配现有数据契约。单handle不可并发调用；多对象部分失败逐对象报告。跟踪具有会话状态，不能当确定性算子缓存。上游track返回固定score=1，不能将其解释成已识别成功或概率；需另验深度一致性、帧时间、跟踪丢失和重新注册。C ABI的存在不等于取消、跨进程恢复、显存预算、Windows构建或生产节拍已经解决。

## 工业小零件的验证要求

| 场景 | 必须区分和记录的结果 |
|---|---|
| 尺度与采样 | 不同零件尺寸、体素/法线邻域、深度量化和噪声；不能沿用室外扫描默认值 |
| 无初值与遮挡 | 任意6DoF初值差、遮挡程度、背景/其他工件干扰；记录成功、拒绝和错误接受 |
| 对称件 | 按显式对称群计算等价误差；非对称判据不足时报告歧义，不伪造唯一朝向 |
| 多实例与无目标 | 稳定身份与重复抑制、相邻工件、正确空结果、ROI为空不回退全云 |
| 交付质量 | 原生float32输出的位移/转角误差、RMSE、模型覆盖率、独立目标支持数和不合格原因 |
| 性能与生命周期 | 模型准备和首帧分别计时，记录稳态耗时、内存、线程预算、取消及重复运行状态隔离 |

软件真值回归验证算法和边界；工业材质、反光、传感器误差、现场误检漏检和节拍仍须代表性CAD/实测点云或RGB-D样本。没有这些证据时，不给出小零件精度或生产可用性的保证，也不把对称等价位姿误写成唯一正确方向。

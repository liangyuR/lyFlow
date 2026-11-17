# 公共轮廓鲁棒拟合与米制量测

`m4-native-profile.lyflow.json` 使用随附合成 CSV；全部节点来自 `std-profile`、`std-geometry`、`std-metrology`。鲁棒拟合需要可选 `profile-robust-kernel`（PCL），此图不需要 Gap 场景包、OpenCV 或 ONNX Runtime。

16 个源槽中有 2 个 NaN 槽、13 个基准线样本及 1 个高度为 2 mm 的参考样本。CSV 明确声明 `mm`、`XZ`、`m4-fixture`；外部 ROI 和锚点以米构造，通过 `profile.bind_geometry` 显式确认同一分析平面。绑定只声明坐标空间，不进行物理标定或自动换轴。

流程为 `profile.input → profile.fit_line_robust / profile.nearest_sample → metrology.flush_point_line / metrology.point_offset`。RANSAC 使用 0.00005 m 内点阈值，保持源槽并排除 NaN 与参考点；`segmentPoints=0` 表示使用完整基准内点。ROI 只用于端点口径，不会替调用方裁剪输入轮廓；本例端点显式选择实际内点两端。

```powershell
lyflow run examples/m4-native-profile.lyflow.json --outputs --summary --no-cache
```

预期 `flush`、`dy`、`distance` 均约为 `0.002 m`；正面差方向显式设为分析平面第二轴。原 Profile 仍为 16 槽、14 有效槽、毫米单位；拟合的唯一内点和最终内点权重数均为 13；锚点最近样本的原槽号及 `sourceIndex` 均为 15。`weightedInliers` 是最终内点及质量统计的多重权重，不表示 RANSAC 的内部求解抽样轨迹。完整索引数组保留在原生载荷，`profile.Selection.v2` 只给出原 cloud.id 及计数。

`embed_profile_workflow.cpp` 使用安装 SDK 的 header-only C++ 客户端，加载同一张图与 CSV，检查共享原 Profile、原槽/索引绑定、原始单位，以及三项读数为有效有限米制值。它不静态链接运行库或 PCL。

```powershell
profile-workflow-consumer.exe C:/installed-lyflow/bin/lyflow_core.dll C:/installed-lyflow/examples/m4-native-profile.lyflow.json
python tests/release/profile_workflow_contract.py --sdk C:/installed-lyflow --cli C:/cli/release/lyflow.exe --output-dir build/m4-profile-delivery
python tests/release/profile_workflow_contract.py --sdk C:/installed-lyflow --cli C:/cli/release/lyflow.exe --runtime-dir C:/public-profile-runtime/bin --public-runtime --output-dir build/m4-profile-public-delivery
```

验证脚本从安装目录独立编译该消费者，将运行库和 release CLI 复制到隔离目录，核对 CLI 实际加载路径，并比较 CLI 与 C ABI 的全部命名输出及浮点位。跨进程载荷身份与存储字节数单独核验后归一，不把不同进程的原生 ID 当作物理来源身份。`--public-runtime` 额外要求原生包集合恰为 `core` 和上述三个公共包，并检查运行目录全部 DLL 及其 PE 依赖闭包，拒绝视觉、推理、匹配和 YAML 等额外后端。验收报告记录图、CSV、运行库的 SHA256；示例只验证合成输入的执行与交付契约，不代表现场测量精度或标定验收。

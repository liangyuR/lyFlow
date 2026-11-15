# 原生帧、Python类型与结果交付

[文档中心](README.md) · [六阶段交付](p2-sequential-delivery.md) · [ADR-0032](adr/0032-native-frames-and-result-delivery.md)

本阶段复用计算节点、共享查看器与宿主接口，补齐原清单15项的通用用途。不是 xyz/Apollo/Cyber、私有录制文件或可视化线协议的兼容层；设备、机器人和现场质量另行验收。

## Python与查看器

SDK直接交换11种端口：PointCloud、Image、Measurement、Record、Tensor、Transform、Plane、Point2D、Line2D、Circle2D、Box2D。大数组使用临时二进制memmap，输入只读，Image/Tensor保留scale。加载与执行均要求协议版本2。标准值/列表、位姿和Features3D使用已有Record契约；可执行五例与16脚本矩阵见[Python分析](p6-analysis-python.md)。

结果查看器支持带ID、帧、方向及尺寸的工件集合，选择实例后查看三维轴/尺寸盒或二维旋转框。云集合按原生cloud ID与Indices配对，不通过相同点数猜身份；只有宿主明确声明固定stride时才把采样点映射回原云。图像叠加要求明确frame、单位、scale和相机/外参，查看与导出使用同一场景。结构化SceneSpec可以输出SVG/PNG；任意SVG XML和Mesh拓扑仍单独立项。完整模式、预算与限制见[查看器分析](p6-analysis-visualization.md)。

`image.render_model_3d`增加overlay、mask、edge_mask、depth、normal模式：mask/edge_mask为U8；depth为相机Z米制F32，0为无命中，并提供有效mask；normal为相机坐标XYZ单位向量F32，只有模型法线可用时产出。缺法线选择normal模式会明确失败；普通模式仍可处理无模型法线输入。STL→表面采样→渲染是点模型组合，不保留网格拓扑。见[渲染器分析](p6-analysis-renderer.md)。

## 完整帧与所有权

`lyflow_frame_capture`从已成功完成的运行取得所有仍可用节点输出、命名结果及原始宿主注入。Frame独立持有原生Data，运行释放和清缓存后仍可读取。`lyflow_frame_run_id`是只读结果视图，既有Cloud/Image/Tensor/Indices/Record getter继续使用；不得将此ID用作计算运行ID。

元信息类型为`lyflow.Frame.v1`。必须提供非空frameId与十进制u64字符串timestampNs；frame未知时为空，calibrationVersion未知时为null。默认CLI时间来自宿主捕获并标记host_capture，不能解释为传感器同步时间。context最多64KiB；原始sourceRunId、展开sourceNodes与中间节点的injectedInputs保留。

归档`.lyframe`是版本化单文件：8字节`LYFR`版本magic、u32LE JSON头长、最多1MiB清单及连续原生二进制。点云各可选通道、图像RGB/RGBA/位深/scale、Tensor/scale、Indices、几何、Measurement包括无效NaN、Record和Bundle均按原值保存。点云别名保持同一新身份；导入分配新native ID，并重绑Indices及已知标准Record声明的点云引用。业务Record内同名字段保持原值，不启发式重写。

默认每帧1GiB，可配置至2GiB，最多8192原生端口/4096命名结果；进程最多1024份独立payload、8GiB逻辑载荷。读取在分配数组前验证范围并预留进程预算；同payload的会话视图共享数组。新文件独占创建临时目标，成功后原子提交，默认拒覆写。取消读写每1MiB检查一次、提交前再次检查，失败清理自己的临时文件。

## FIFO与副作用

原生Session、C++ FrameSession、Rust Session都使用同一有界FIFO，与计算结果缓存独立。配置id、capacity（1–1024）、maxBytes（至2GiB）、overflow（drop_oldest/reject/drop_newest）、timestampOrder（nondecreasing/arrival）、schemaChange（reset/reject）。默认16帧、256MiB、队首淘汰、时间不倒序、结构变化重置。

同ID的不同Session通过epoch隔离。reset先清空再接纳当前帧，成功后分配新epoch，sequence单调继续；类型、命名结构、坐标帧或标定版本改变同样按schemaChange处理。取出全部帧不会重置时间顺序游标。已取出的Frame跨reset和Session关闭继续有效，原Frame不被push改写。取消或拒绝入队保持队列/epoch/序列不变；front可peek或pop。C ABI操作串行受锁保护，宿主应在连接关闭时停止生产者再释放会话句柄。

## 可执行交付链

```powershell
lyflow record examples/p6-delivery_demo.lyflow.json --recording build/new-recording --count 2 --frame world --calibration-version demo-v1
lyflow replay build/new-recording
lyflow replay build/new-recording --recompute
lyflow log build/new-recording --node pose3d.pose --output build/poses --log-kind poses3d --length-unit mm --pose-format ZYX --angle-unit deg
```

record保存有效图、参数、逐帧归档和小索引。默认replay只读；重算只注入展开计划的源结果与原始中间端口注入，让计算节点真正执行。替换图必须显式绑定，缺失源结果会失败，不假装成功。支持时间/序列筛选与按时间节奏回放。

`publish`只发送到显式TCP端点，包含完整归档与版本化元信息；接收器验证并持久化后返回匹配frameId、session ID/epoch/sequence的ACK。完整发送后断线、超时或取消仍为未确认，不自动重试；下一次显式发布才重连。仅使用本机loopback验收。线格式、接收器命令、日志24轴欧拉约定/首项/全部/固定精度/UTC日归档及冲突策略见[宿主交付分析](p6-analysis-delivery.md)。

C++17示例为`examples/p6-delivery_host.cpp`，使用安装SDK的`lyflow/frames.hpp`和动态库；Rust客户端以Arc保留Core。Frame、结果View与运行句柄保留动态库所有权，可晚于Client/Core外部变量销毁。新增符号均按需绑定，旧1.3运行库能继续加载；新能力调用才报缺少符号。

## 验证入口

- `core/tests/test_frames.cpp`：原生全类型、可选通道、位深、NaN、只读所有权、身份、预算、坏归档、FIFO顺序/容量/取消/重置。
- `core/tests/test_python.cpp`与`python/tests/`：双端协议、实际worker、11类端口、单位/scale、非法输出与恢复。
- `bridge/tests/p6_delivery_contract.py --cli <CLI> --core <DLL> --output-dir <目录>`：真实点云/图像/Tensor/Indices/Record逐字节往返、只读/重算、FIFO、日志与外部loopback接收器。
- 共享可视化及编辑器既有功能组：身份、坐标、导出与错误提示；React18/19正式类型检查。编辑器总数保持100以下且不新增跳过。

软件门禁不证明传感器同步、生产精度、遮挡/对称件质量、现场节拍或长期稳定性。

本地最终原生全量门禁：698组、375098断言，零失败、零跳过；包含全部七个包、真实Python worker及选定检测/分割/OCR模型。独立安装1.3.12 SDK的Release CLI/C++17原生帧验收、前阶段8个SDK工作流及缺失OpenCV诱导加载负例通过；同8图CLI默认/覆写/恢复共24次也通过。这些是软件契约验证，不替代上述现场验收。

编辑器完整94项与共享可视化7项均通过且零跳过，React18/19类型检查通过；跨运行底图/叠加/导出一致性及空掩膜分配预算已由交叉审查补回归并复核。CI和机器审查状态见[PR #59](https://github.com/liangyuR/lyFlow/pull/59)。

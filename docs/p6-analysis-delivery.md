# P6 通用宿主交付分析

本轮逐项阅读本机 `xyz-vision-lib` 的 Output 模块、CacheData 及 CommonScripts 日志/回放实现和 property_settings。目录没有 Git 元数据，不能证明与外部清单指定修订一致；仅分析语义，LyFlow 独立实现，不复制专有函数体。

## 原用途与交付契约

| 原项 | 实际参考用途 | LyFlow 交付要求 |
| --- | --- | --- |
| BasicDatatypesPub | 单个 Int/Double/Bool/String，经 workspace 前缀与 topic 向 Apollo Cyber protobuf writer 发布 | 版本化命名结果，保留标准值种类、单位、topic 与会话身份；真实端点、确认、超时及断线语义 |
| Primitive2DPub | 发布有序二维实例集合，包含姿态、旋转框和掩膜 | 实例 ID/帧/单位和集合顺序保留，原生掩膜不塞 JSON，不根据数组位置替代稳定身份 |
| Primitive3DPub | 发布三维实例名称、ID、置信度、父帧、姿态、尺寸及附加信息 | 三维米制、xyzw 四元数、ID/帧及附加信息保留，支持批量和空结果 |
| OutputPub | 组装 Response，而非独立发送；预定义工件/字符串/云/图像以及 dynamic 命名基本值/二维几何 | 统一命名结果帧承载全部原生类型与结构化结果，由宿主选择文件归档或端点，不将 JSON 摘要冒充真实点云/图像 |
| LogString | 在默认日期日志目录追加 HHmmss 与输入字符串 | 明确 UTC/时钟域与文件路径、结构版本、冲突/追加和取消，换行字符串保留而不破坏 JSONL 边界 |
| LogPrimitive2D | 只取首个二维工件，固定精度输出 x/y/角度，可选 rad/deg | 明确 first/all 选择，保留实例身份；px/m 单位不隐式互换，角度转换显式 |
| LogPrimitive3D | 首个三维工件；m/mm、quat 或全部合法内/外旋 Euler 轴序、rad/deg | 完整轴序与四元数格式，固定精度及奇异性标记；不把未知姿态默认成 identity |
| RecordReplayer | 扫描 timestamp/workspace requests.json 与对应 responses.json，切换离线 recipe/虚拟设备，再调用外部 bridge | LyFlow 自有时间序列录制，保存真实原生载荷、时间戳/帧/标定版本，按帧/时间选择和节奏回放，显式重绑图端口；不隐式切换用户当前配方或假装外部设备已适配 |

外部 Cyber/protobuf 生成定义、writer runtime 和设备桥并未随本 repo 提供。本阶段按已批准 D01 交付 LyFlow 通用宿主协议；是否逐字兼容 xyz bridge/viz 原协议是原计划独立适配决策，不能把本协议标成原协议实现。

## 不可复用为完整录制的旧快照

当前 CLI snapshot 可以捕获点云 PCD、图像 LYIM 和小 JSON 结果，但 Tensor/Indices 只有检查摘要，也没有完整读回/原生注入。新录制必须围绕统一原生帧 codec，不用该快照或重新运行原图假装已回放原始载荷。

## 端点与副作用

通用发布使用显式选择的端点，消息包含版本、会话/帧/序号、时间戳、topic、坐标帧/标定版本及实际二进制载荷。只有匹配 ACK 才算交付；断线或 ACK 超时为未确认，不能自动重发一份可能已被接收的副作用。重连由下一次显式发布触发；取消在可中断 I/O 边界检查。端点测试仅使用自建 loopback 接收器，不连接外部生产服务。

日志和归档路径由宿主配置，默认拒绝覆盖已有目标；追加是明确选项。输出文件数量/字节有界，不能在计算 DAG 内启动无限录制循环。传感器同步、设备协议、PLC、现场节拍和长期稳定性仍须独立验收。

## 可执行宿主入口

`lyflow record examples/p6-delivery_demo.lyflow.json --recording <新目录> --count 3 --interval-ms 50 --frame world --calibration-version demo-v1`
保存载入并应用配方/参数后的 `graph.lyflow.json`、`effective-params.json`、原始请求参数、展开执行计划中 level=0 的 sourceNodes，及逐帧 `.lyframe`。索引类型是 `lyflow.Recording.v1`，时间戳与序号使用十进制 u64 字符串。默认 `context.timestampKind=host_capture`；`--timestamp-ns` 只允许单次录制并标记 explicit。未指定坐标帧为空、标定版本为 null，不能解释为已经标定。索引 complete=false 表示中断前已持久化的前缀，仍可读取；完成后才写 complete=true。

`lyflow replay <目录>` 只读取保存的原生结果，不执行算法；`--recompute` 使用保存的图与源输出作原生注入，计算节点重新运行。替换目标图必须 `--graph ... --bind sourceNode.sourcePort=targetNode.targetPort`，不通过同名猜测绑定。`--from-sequence/--to-sequence` 与 `--from-timestamp-ns/--to-timestamp-ns` 为包含边界的过滤，可用 `--paced` 按原始相对时间回放。缺失源结果、非法引用、损坏归档或不同帧/标定索引会报错。归档不打包外部算子库和第三方模型，重算依赖由宿主提供。

自动重算只绑定未标记 `virtual:true` 的实际输出端口，Bundle 的虚拟字段由完整 Bundle 载荷恢复，不根据端口是否含点号判断。显式绑定可从 Bundle 字段读取：例如 `--bind scan.scan.merged=source.cloud`；源地址按元信息中的真实 nodeId/port 对解析，存在歧义时可用 `node:port` 明确分隔，目标端口也支持这种写法。

`lyflow log <目录> --node pose3d.pose --output <日志路径> --log-kind poses3d --length-unit mm --pose-format ZYX --angle-unit deg`
默认 JSONL，每行保留时间戳/帧/标定与原始实例。`--selection first|all` 显式控制旧脚本的首项习惯；m/mm 固定5/2位、px固定4位、角度 rad/deg 固定5/3位、四元数固定5位。24种轴序支持内禀大写与外禀小写，奇异点返回等价旋转并标记 gimbalLock；约定参考 [SciPy Rotation.as_euler](https://docs.scipy.org/doc/scipy/reference/generated/scipy.spatial.transform.Rotation.as_euler.html)，用独立矩阵重建测试验证。`--log-format text` 保留逐行 JSON 转义值，换行字符串不会拆成假记录。默认按 UTC Unix 日分组为 `<日志路径>.day-<Unix日>`，也可 `--log-archive none`；文件保留由宿主管理，本轮不会自动删除历史。默认 conflict=fail，追加需要 `--conflict append`，目标独占锁与 max-bytes 上限阻止并发写冲突和无界增长。

`python examples/p6-delivery_receiver.py --core <core路径> --directory <接收目录> --port 54547 --count 3 --topic inspection`
启动独立接收器；随后 `lyflow publish <录制目录> --endpoint tcp://127.0.0.1:54547 --topic inspection`。端点要求数值 IPv4 或带方括号 IPv6，避免 DNS 阶段不可取消。每帧显式连接一次，不隐式重发；再次调用命令才重连。接收器调用同一个 core 的 archive load 完整验证真实原生载荷，持久化后才 ACK。

发布线格式为 magic `LYFPUB1\0`、u32LE header长度、u64LE payload长度、UTF-8 JSON header、原样 `.lyframe` 字节。header 上限2MiB、ACK上限64KiB、默认归档上限1GiB，可调至2GiB。header 类型 `lyflow.Publish.v1`，包含 topic、frameId、metadata、payloadFormat、十进制 payloadBytes、payloadSha256。ACK 为单行 `lyflow.PublishAck.v1`，schemaVersion=1，frameId与 session 的 id/epoch/sequence（若有）精确匹配，accepted=true 才算确认。完整发送后断线/超时/取消均为交付未确认，调用方必须自行决定是否再次发布。SHA只证明文件完整性，不是产品身份或认证。

Rust `Frame`/`Session` 持有 `Arc<Core>`，保留动态库及原生所有权。新 C ABI 只按需绑定，旧1.3补丁仍可加载；缺少能力时调用返回 MissingSymbol。Frame的只读viewId可用既有 Cloud/Image/Tensor/Indices/Record getter，回放不将大数组塞进 JSON。索引8MiB、单次最多10000帧、每次I/O检查取消；Windows Ctrl+C 同时覆盖执行与两次运行间的等待/发布。

C++17 对应 `lyflow::Frame` / `lyflow::FrameSession`，通过 `Client::captureFrame/loadFrame/createSession/startRunWithFrame/runWithFrame` 使用；独立头 `lyflow/frames.hpp` 随SDK交付，所有帧、会话和原生视图持有共享动态库租约，晚于 Client 析构仍可读取。新接口与输入感知 `effectiveParams` 按需找符号；带取消回调的 load/save/push 使用 `_ex` 入口，在原生大数组分块读写及会话提交前检查，回调异常按取消处理。

录制总原生归档预算默认4GiB，可设 `--max-total-bytes`（≤64GiB）；单帧预算默认1GiB（≤2GiB）。超额帧不会写入索引，之前已提交帧保留。重算恢复原始输入注入，包括非源节点的输入端口；具有input-only注入的level=0计算节点重新执行，使用帧中独立保存的原始输入。输入感知的validate/plan/effective-parameters与运行一致，不把“缺少运行时输入”的诊断数组冒充生效参数。

`python bridge/tests/p6_delivery_contract.py --cli <lyflow路径> --core <core路径> --output-dir <验收父目录>` 完整执行：真实100000点与强度、图像、张量、Indices和Record经归档/原生getter/loopback端点逐字节比较；FIFO容量/序列/epoch/取消所有权、日志冲突/追加/归档；非源输入注入后重算下游100003点并保留Measurement/Transform。另以实际 `register.global_3d` 消费保存后重新加载的模板/模型，并由 `cloud.statistics`、`value.field`、`value.compare` 消费可见面cloudId，覆盖原生身份重绑。C++例子 `examples/p6-delivery_host.cpp` 单独编译，可验证Frame/会话/视图跨Client生命周期及取消push不改变队列；`--legacy` 验证旧补丁仍能加载。

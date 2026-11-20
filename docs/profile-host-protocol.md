# Profile 宿主协议

[文档首页](README.md) · [嵌入指南](embedding.md) · [HTTP 契约](http-transport.md) · [版本与交付规则](deployment/packaging.md)

Profile 是带明确坐标、单位、顺序与来源契约的原生轮廓类型。XYZ 及可选强度、法线、RGB 通道由只读 PointCloud 缓冲承载，另有逐槽有效性和原始采样索引。运行库内通过共享所有权复用缓冲；C ABI、C++ / Rust 客户端、Frame、Python 与编辑器均保留该类型。大型数组不放入 JSON 或 Record。

## 版本与能力协商

产品与 C ABI 从 `1.3.12` 升到 `1.4.0`。`lyflow_run_options` 末尾新增 `profile_inputs` 与 `profile_input_count`，结构尺寸发生破坏性变化。旧 `1.3.x` 宿主不能把旧结构直接交给 `1.4.x` 的 `lyflow_run_start` 或 `lyflow_run_start_frame`，即使本次没有 Profile。必须用匹配的头文件或 Rust 客户端重新编译，并初始化完整结构。

C++ / Rust 动态客户端先检查引擎与 ABI 的主、次版本，同一 `1.4.x` 系列允许补丁兼容，跨系列拒绝加载。直接使用 C ABI 的宿主也须在调用其他接口前检查 `lyflow_version()` 与 `lyflow_abi_version_string()`；包元数据与正式资产的精确版本检查仍遵循[发包规则](deployment/packaging.md)。

`lyflow_runtime_capabilities()` 声明 `profiles: true` 与 `profileTransfer: "ordered-slots-v1"`。它表示原生类型和传输契约可用；具体适配算子是否编入仍须检查 manifest。`std-profile` 提供 `profile.from_cloud`、`profile.to_cloud`、`profile.remap_axes`，不能仅凭编辑器支持就推断某个运行库具备这些算子。自定义 Transport 的 `getOutputProfile` 是可选能力，未实现时应明确提示缺少能力。

## 数据契约

Descriptor 必须恰好包含以下八个字段，JSON UTF-8 文本最多 8 KiB：

| 字段            | 约束                                                      |
| --------------- | --------------------------------------------------------- |
| `schemaVersion` | 整数 `1`                                                  |
| `frame`         | 显式坐标帧，非空、不含 NUL，最多 1024 个 UTF-8 字节       |
| `unit`          | `m` 或 `mm`                                               |
| `plane`         | `XY`、`XZ` 或 `YZ`                                        |
| `handedness`    | `right` 或 `reflected`                                    |
| `sampleOrder`   | `unknown` 或 `acquisition`                                |
| `sourceId`      | 显式采样来源身份，非空、不含 NUL，最多 1024 个 UTF-8 字节 |
| `sourceCount`   | JSON 整数，范围 `0..4294967295`                           |

`plane` 和 `handedness` 声明数据解释方式，读取 Profile 不会自动换轴、反射或丢弃第三维。轴映射与单位转换应通过显式适配算子执行。

对于 N 个槽：

- XYZ 为交错 `float32[3N]`。强度为可选 `float32[N]`，法线为可选 `float32[3N]`，RGB 为可选 `uint8[3N]`，顺序为 R、G、B。
- `validity` 为 `uint8[N]`，只允许 0 或 1。有效槽的 XYZ 必须有限；无效槽允许 NaN，不允许 Inf。掩码不会触发隐式压缩。
- `sourceIndices` 为 `uint32[N]`，无重复且小于 `sourceCount`。`acquisition` 要求严格递增；`unknown` 允许重排，不代表已确认采样顺序。
- 空轮廓允许 N 为 0；`sourceCount` 可以为 0，也可以保留原始采样总数。N 不得超过 `sourceCount`。

`sourceId` 表示采样来源；carrier 的 `cloud_id` / `sourceCloudId` 是运行库分配的点云缓冲身份，用于索引关联。两者不能互换。后者在跨进程载入 Frame 时会重绑定，不是持久化业务标识。

## C ABI 输入与读取

`lyflow_run_profile_input` 包含绑定 `node_id` / `port`、槽数、各通道指针、Descriptor JSON、掩码、源索引，以及可选稳定句柄 `handle`。真正执行注入时节点和端口必须非空；单独创建稳定载荷时允许先不指定绑定。

| 接口                                | 用途                                          |
| ----------------------------------- | --------------------------------------------- |
| `lyflow_run_options.profile_inputs` | 普通运行和 Frame 运行携带 Profile             |
| `lyflow_plan_inputs_ex`             | 带 Profile 的计划及缓存预测                   |
| `lyflow_effective_params_inputs_ex` | 带 Profile 的生效参数解析                     |
| `lyflow_cache_evict_inputs_ex`      | 按同一注入身份清理结果缓存                    |
| `lyflow_profile_input_create`       | 从 raw 数组创建不可变稳定载荷                 |
| `lyflow_profile_input_capture`      | 从现有 Profile 输出捕获稳定载荷，保持缓冲身份 |
| `lyflow_profile_input_free`         | 释放稳定载荷                                  |
| `lyflow_output_profile`             | 取得只读 Profile 切片视图                     |
| `lyflow_profile_view_free`          | 释放视图及其共享所有权                        |

原来的点云/图像 `*_inputs` 入口保留；需要 Profile 时使用 `*_inputs_ex`。同时注入多个数据类型时仍遵守已有绑定规则：输出端口注入会跳过该节点的 compute，输入端口注入则正常执行 compute；同一节点不能混用输出注入与输入注入。完全覆盖所有声明输出时，未参与执行的参数校验与算子 validate 钩子跳过；只覆盖部分输出时仍保留参数校验，未覆盖的输出不存在，接在其上的可选输入按未连接处理，必填输入报 `missing_input`。算子存在性、版本与图结构继续校验。

Raw 输入会在调用期间复制到引擎自有缓冲，调用返回后可以释放原数组。原始指针必须覆盖声明的元素数量，C ABI 无法从裸指针推断实际分配长度。语法错误、必需缓冲为空等入口错误明确拒绝；合法 JSON 但违反单位、字段、槽、索引等数据契约的输入，统一在运行、计划、生效参数与清缓存路径返回 `bad_input` 诊断。

### 稳定句柄与缓存

每次重新复制 raw Profile 都会产生新的 carrier 点云身份。即使数值相同，也不能把独立 raw 调用当成同一来源来预测或清理先前缓存。跨请求复用时先调用 `lyflow_profile_input_create`，后续 run、plan、effective 与 cache 接口引用同一个 `handle`。设置 `handle` 后忽略该项的数组和 Descriptor 字段，`node_id` / `port` 仍决定本次注入位置。

`create`、`capture` 成功返回已分配的空字符串并设置输出句柄；失败返回错误文本或空指针，输出句柄为空。返回的文本由 `lyflow_string_free` 释放。稳定句柄只属于创建它的运行库实例，宿主应保留对应 DLL 的生命周期；不要将不透明指针发送到另一个进程或热重载后的另一份运行库。

输入调用返回后，运行或计划已持有自己的共享所有权，可以释放调用方句柄。若还要进行之后的缓存操作，则继续保留它。`capture` 可以让已有输出成为下次运行的输入，无需复制数组。缓存键仍包含算子版本、参数与其他运行语义；复用句柄不会把不同参数的任务混为同一个任务。

### 输出视图

`lyflow_output_profile(run, node, port, offset, count, &view)` 返回连续原始槽。`count=0` 表示读取到末尾；越界 offset 产生空切片，实际 offset 截到槽总数。视图提供 `point_count`、`total_points`、`offset`、通道 flags、carrier `cloud_id`、纯 Descriptor JSON 和各缓冲指针。

读取没有隐式采样或压缩，切片的 `source_indices` 仍是原始采样索引。各指针及 Descriptor 保持有效直到 `lyflow_profile_view_free`，即使原 run 被释放或缓存被清理。返回码为 0 成功、1 输出不存在或类型不符、2 输出视图指针为空、3 异常。空轮廓成功时允许数组指针为空。

## C++、Rust 与 Frame

| 能力                   | C++ SDK                                                 | Rust 客户端                                                                         |
| ---------------------- | ------------------------------------------------------- | ----------------------------------------------------------------------------------- |
| 载荷描述               | `InputProfile`                                          | `RunProfileInput`                                                                   |
| 创建稳定载荷           | `Client::retainProfileInput`                            | `Core::retain_profile_input`                                                        |
| 捕获输出为输入         | `Client::captureProfileInput`                           | `Core::capture_profile_input`                                                       |
| 引用稳定载荷           | `InputProfile::retained`                                | `RunProfileInput::retained`                                                         |
| 运行输入集合           | `RunOptions::profileInputs`                             | `RunSpec::profile_inputs`                                                           |
| 精确输出切片           | `Client::profile`、`RunHandle::profile`                 | `Core::output_profile`                                                              |
| Frame 读取             | `Frame::profile`                                        | `Frame::profile`                                                                    |
| Frame 输出作为稳定输入 | `Frame::profileInput`                                   | `Frame::profile_input`                                                              |
| 计划/参数/清缓存       | `plan`、`effectiveParams`、`evictCache` 的 Profile 参数 | `plan_with_profiles`、`effective_params_with_profiles`、`cache_evict_with_profiles` |

`ProfileInputHandle` 与 `ProfileView` 通过 RAII 管理原生资源和运行库生命周期。数组视图只读；需要修改数据时应创建新的输出载荷。C++ 客户端仍只依赖 `client.hpp` / C ABI，不要求宿主采用引擎内部的数据类或 JSON 库。

Frame 捕获和原生归档保留 Profile、掩码、源索引及共享点云关系。录制/回放与 Frame 绑定继续走相同原生类型。归档载入会重新分配进程内 carrier 身份；加载后在同一 Frame 或由它捕获的稳定句柄上复用缓存，不应根据原文件中的旧 `cloudId` 直接查当前进程缓存。

## LYPR 二进制布局

Tauri、HTTP 和 CLI `.lypr` 共用 LYPR v1。所有整数和 float32 均为小端；前 32 字节是八个 uint32：

| 字节偏移 | 字段          | 值                                                                  |
| -------- | ------------- | ------------------------------------------------------------------- |
| 0        | magic         | ASCII `LYPR`，整数 `0x5250594c`                                     |
| 4        | version       | `1`                                                                 |
| 8        | count         | 本次连续槽数 N                                                      |
| 12       | totalPoints   | 完整轮廓槽数                                                        |
| 16       | offset        | 本次起始槽                                                          |
| 20       | flags         | bit0 强度、bit1 法线、bit2 RGB；其他位保留                          |
| 24       | metadataBytes | JSON envelope 区原始字节数，最多 16384；内层 Descriptor 仍最多 8192 |
| 28       | reserved      | `0`                                                                 |

随后依次排列：

1. UTF-8 JSON envelope：`{"descriptor":{八字段},"cloudId":"uint64十进制字符串"}`，补零到 4 字节边界。
2. XYZ：`float32[3N]`。
3. 若 flags bit0 为 1：强度 `float32[N]`。
4. 若 flags bit1 为 1：法线 `float32[3N]`。
5. 若 flags bit2 为 1：RGB `uint8[3N]`；随后补零到 4 字节边界。
6. `validity`：`uint8[N]`，随后补零到 4 字节边界。
7. `sourceIndices`：`uint32[N]`。

Envelope 与 Descriptor 都按各自固定键集合校验。`cloudId` 使用字符串避免 JavaScript 丢失 uint64 精度；`sourceCount` 仍是 uint32 范围内的 JSON 整数。NaN 通过二进制数组保留，不写成 JSON 数值。接收方应验证版本、flags、元数据与数组长度、切片范围及每个槽的契约，拒绝截断或尾随载荷。

该格式是结果传输格式。M1 没有把 `.lypr` 增加为 CLI 的直接输入文件格式，也不把它当作包含工作流和命名输出的 Frame 归档。

## CLI 与 Transport

导出 Profile：

```powershell
lyflow dump graph.lyflow.json nodeId:profile output.lypr --format profile
```

`dump` 支持已有的 `--input source.port=cloud.pcd` 点云注入选项，可在图中通过 `profile.from_cloud` 显式声明坐标与来源后导出。普通 `run --snapshot-dir <new-dir>` 会按捕获策略把 Profile 保存为 `.lypr`，在 `snapshot.json` 中记录类型、文件和大小，受相同的配额与失败保留规则约束。

编辑器 Transport 使用可选 `getOutputProfile(runId, nodeId, port): Promise<ArrayBuffer>`。默认请求完整槽集合，不传 `maxPoints`，不按普通点云预览逻辑抽样：

| 宿主          | 入口                                                               |
| ------------- | ------------------------------------------------------------------ |
| Tauri         | `get_output_profile({runId,nodeId,port})`，返回二进制 IPC Response |
| HTTP 当前运行 | `GET /lyflow/runs/{runId}/profiles/{nodeId}/{port}`                |
| HTTP 快照     | `GET /lyflow/snapshots/{snapshotId}/profiles/{nodeId}/{port}`      |

HTTP 载荷使用 `application/octet-stream`，URL 路径段须分别编码。仓库中的 HTTP test-server 从捕获快照读取 LYPR，外部宿主需要自行实现相同接口；这些路由不意味着运行库自动启动 HTTP 服务。

## 验证范围

随SDK安装的[embed_profile.cpp](../examples/embed_profile.cpp)使用稳定输入句柄注入三槽Profile，验证NaN、有限无效槽、源索引及米制导出单位。它通过独立安装目录的`find_package(lyflow 1.4.0 EXACT CONFIG REQUIRED)`及`lyflow::client`编译为C++17消费者，并已实际执行；不依赖源码树私有头。运行时需包含`std-profile`。随包[工作流](../examples/m1-profile-adapters.lyflow.json)可通过桌面或CLI运行。

[原生 C ABI 回归](https://github.com/liangyuR/lyflow-engine/blob/main/core/tests/test_profile_c_api.cpp)检查原始缓冲复制、NaN 槽、精确切片、视图生命周期、稳定句柄的 run/plan/capture/cache 一致性，以及非法数据和空绑定诊断。[Rust / CLI 集成](../bridge/tests/profiles.rs)实际加载 Windows 1.4.0 DLL，检查 Rust 输入、Frame 捕获、缓存预测/清理、LYPR 布局、CLI 点云适配导出与快照落盘。[Profile 原生测试](https://github.com/liangyuR/lyflow-engine/blob/main/core/tests/test_profile.cpp)另覆盖数据契约、摘要与原生归档；编辑器解码回归见 [profile.test.mjs](../packages/editor/test/profile.test.mjs)。

本轮宿主协议验证使用 Windows x64、本地 CPU 构建与模拟输入。C++ SDK 已完成独立 C++17 编译检查；Rust / CLI 路径执行真实 DLL 集成。它们不能替代正式 SDK ZIP / NuGet 安装验收、跨平台运行、实际相机或工业轮廓数据验收，也不能单凭 LYPR 编解码测试宣称浏览器或桌面完整交互流程已验收。当前发布组合与全量门禁状态见[发包规则](deployment/packaging.md)和[测试地图](testing.md)。

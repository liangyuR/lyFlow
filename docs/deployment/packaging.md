# Windows x64 运行库与 SDK 发包

## 发布契约

`pnpm run package` 从当前源码构建并验证两个 ZIP。本地入口不上传文件；GitHub 的 `Windows release` 工作流使用同一入口。首版交付运行库和 headless CLI/SDK，不生成桌面安装程序，不包含客户模型、客户图像、配方、历史记录或调试符号。

| 资产 | 布局与消费方式 |
|---|---|
| `lyflow-runtime-win-x64-vX.Y.Z.zip` | 根目录包含 `lyflow_core.dll`、必要依赖及 VC++ CRT、`lyflow-runtime.json`、许可证。解压目录可作为 GlueSight 的 `-LyFlowRuntime` 输入。 |
| `lyflow-sdk-win-x64-vX.Y.Z.zip` | `bin/` 包含同一份运行库及 `lyflow.exe`；另含头文件、导入库、CMake 配置、示例和 `library/`。 |

算子组合固定为 `std-pointcloud`、`std-image`、`std-ml`、`glue`，不接受环境变量追加 `gap`、`dts` 或外部算子。ONNX Runtime 使用 CPU 版 `1.19.2`。PCL 最低支持版本为 `1.15.1`，发包清单固定到 `1.15.1#1`。core 使用 RelWithDebInfo 与动态 CRT；CLI 使用 Rust Release 和 `--no-default-features`。SDK 从同一份 core 暂存文件生成，避免两套资产实际包含不同引擎。

本版 SDK 的 C++ 客户端与同一发布修订的 Rust 客户端，在 Windows 上将依赖搜索目录限制为指定 DLL 所在目录，不读取 PATH，也不从当前工作目录或 System32 补齐第三方运行库。Windows KnownDLL 与 API Set 系统组件仍由操作系统解析。宿主需把所有非系统依赖部署在 DLL 目录中；缺依赖时应明确失败。GS 当前固定旧修订客户端的加载行为没有被本次修改，旧客户端兼容性测试不代表它获得了这项加载限制。

产品版本统一为 `X.Y.Z`，ABI 独立编号。`lyflow-runtime.json` 记录 `engineVersion`、`abiVersion`、`sourceRevision`、架构、构建配置、算子包及版本、实际算子、依赖版本和相对文件清单。最终 DLL 查询结果必须与声明一致。不使用内容哈希或指纹建立包身份、匹配或校验；Git 提交标识仅用于源码修订。

Rust 客户端继续从同一发布提交引用，不附进 ZIP、不发布 crates.io。Release 提供完整 `rev` 的 Cargo 示例。GS 使用当前固定客户端的兼容性由发布消费验证单独检查；ABI 不兼容时必须更新客户端，不能宣称运行库可直接替换。下游应保留已验收运行库及随包 CRT，不能无条件用其他版本覆盖。

## 本地入口

前置工具：Windows x64、PowerShell 7、Visual Studio 2022 C++ 工具集和 Windows SDK、CMake/Ninja、Git、仓库声明的 Rust/Node/pnpm，以及 Python 3.12。Python 门禁依赖使用 `scripts/requirements.txt`。

```powershell
pnpm install --frozen-lockfile
python -m pip install -r scripts/requirements.txt
pnpm run package -OutputDir C:\release\lyflow
```

源码必须干净且有有效 Git 修订；不要在正在编辑的工作树里制作正式产物。正式 tag 构建可加 `-ExpectedTag vX.Y.Z`；脚本校验版本，不自动修改源码版本。输出目录应位于仓外或仓内已忽略的构建目录；默认是 `build/release`。已有同名产物时拒绝覆盖，复验构建须选新输出目录。`-AllowDirty`、`-SkipDependencySetup` 只用于本地诊断，tag 发布禁止使用。

入口准备固定版本依赖、构建 core 与 CLI、运行门禁、生成暂存目录和 ZIP，并验证解压后的交付物。vcpkg manifest 固定 baseline；构建不从相邻项目复制依赖。vcpkg 工具和安装树分别位于 `build/release-tools/vcpkg`、`build/release-deps/vcpkg_installed`，二进制缓存位于 `build/release-deps/binary-cache`。ONNX Runtime 位于 `third_party/onnxruntime/onnxruntime-win-x64-1.19.2`。GitHub 分别缓存 vcpkg 二进制缓存和解压后的 ONNX Runtime。vcpkg 使用 `windows-2022`、x64、完整 MSVC toolset 版本、vcpkg baseline 和发布组合修订 `std-glue-v2` 构成稳定键；同一身份不再按运行编号重复保存，也不跨身份前缀恢复。调整依赖 features、triplet 或发布组合时必须同步更新缓存身份；`v2` 隔离旧的可能不完整缓存。ONNX Runtime 按 Windows x64、CPU 版和 `onnxruntimeVersion` 精确缓存。恢复后仍运行固定依赖版本和必需文件检查；所有依赖准备成功后立即保存未命中的缓存，再执行同一个 `pnpm run package` 完整构建及验收。依赖准备失败不保存不完整缓存，后续软件门禁失败不影响已保存依赖。缺缓存时仍须能从固定输入完整构建。

独立复验已有 ZIP：

```powershell
pwsh -NoProfile -File scripts/test-release.ps1 -ArchiveDir 'C:\验收 包' -ReportPath 'C:\验收 包\release-verification.json'
```

独立验收不要求 vcpkg 安装树。缺失必测依赖、夹具或模型即失败，不把跳过算作成功。失败报告与 CI 日志要保留，不能只保留成功日志。工作流在成功或失败后均保存 `windows-release-build-diagnostics` 和 `windows-release-verification-diagnostics`，保留 14 天；只收集 JSON/日志，不上传解压 DLL 或重复 ZIP。报告中的 `evidenceDirectory` 是临时 runner 路径，对应的探针 JSON 和日志可从诊断 artifact 中取得，避免 runner 销毁后丢失证据。若工具安装等前置步骤失败、尚未产生报告，则以该次 Actions 日志为准。

## GitHub 门禁与正式发布

| 触发 | 行为 |
|---|---|
| PR | 原有 CI 的 `fast`、`native` 不变。 |
| main push | 原有 CI 加完整 Windows 发包与独立 runner 消费验收；不创建 Release。 |
| 手动 `Windows release` | 可选择实现分支，运行完整门禁，保存 ZIP、清单、验证报告 14 天；不创建 Release。 |
| `vX.Y.Z` tag push | tag 必须与源码产品版本一致，且提交位于 main 历史；门禁成功后创建草稿 Release，人工正式发布。 |

完整构建和独立消费验收都使用 GitHub 托管 `windows-2022`；编译 job 仅有 `contents: read`。独立验收 runner 不继承编译工作目录或已准备的依赖树。发布 job 只下载已通过门禁的资产，再以 `contents: write` 创建草稿并上传；不在持有写权限时编译或运行库代码。

首次合并前，可从已经注册的 `CI` 工作流选择实现分支手动触发；它调用同一套完整发包流程。这个仅负责调用的 job 声明 `contents: write` 作为可重用工作流权限上限，实际 build/verify job 显式保持只读；手动事件不进入 publish job。新增工作流尚未进入默认分支时，可用 `gh workflow run ci.yml --ref <实现分支>` 验证。

正式发布顺序：先合并经过审查的版本改动并确认 main 门禁成功，再推送匹配版本的 tag；检查草稿资产、ABI、算子组合、客户端修订及验证报告，最后人工发布。GS 仅引用已正式发布的固定 tag 和资产名，不使用 `latest`。签名和自动更新不属于首版。

工作流拒绝任何已存在同版本 Release，包括草稿，不覆盖旧资产。若上传中断，保留草稿供排查，禁止通过重跑覆盖；采用新产品版本重新发包。源码、依赖或交付文件改变，都必须增加产品版本。

## 验收范围与证据

发布门禁针对最终 ZIP：core 单测、manifest 自检、ABI/版本/架构、点云与图像处理、标定、胶路宽度/断胶、ONNX CPU 实际推理；外部 C++ SDK 工程与 Rust 客户端消费、Release CLI；中文和空格路径、不同工作目录；实际加载模块来源；缺 DLL、依赖、错误 ABI/架构/元数据的失败行为。必测项不能静默跳过。

本文件描述实现契约和门禁，不构成任何一次构建的通过证据。实际通过必须以对应源码修订的 GitHub 运行、随包清单和验证报告为准；新增脚本或 YAML 静态检查通过不代表干净 runner 的完整构建通过。历史测试数字也不能替代本轮证据。

GlueSight 安装包与静默安装验收由 GS 工作流负责。客户图像效果、真实设备、现场驱动、节拍和长期稳定性另行验收；合成夹具及软件消费测试不能代替现场结果。

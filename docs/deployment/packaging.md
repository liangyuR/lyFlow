# Windows x64 运行库与 SDK 发包

## 发布契约

`pnpm run package` 从当前源码构建两个 ZIP 与编辑器 tarball，并检查暂存布局与 DLL 身份。本地入口不上传文件；GitHub 的 `Windows release` 工作流使用同一入口。首版交付运行库和 headless CLI/SDK，不生成桌面安装程序，不包含客户模型、客户图像、配方、历史记录或调试符号。

| 资产 | 布局与消费方式 |
|---|---|
| `lyflow-runtime-win-x64-vX.Y.Z.zip` | 根目录包含 `lyflow_core.dll`、必要依赖及 VC++ CRT、`lyflow-runtime.json`、许可证。满足 GlueSight 必需算子及其独立验收后，解压目录可作为 `-LyFlowRuntime` 输入。 |
| `lyflow-sdk-win-x64-vX.Y.Z.zip` | `bin/` 包含同一份运行库及 `lyflow.exe`；另含头文件、导入库、CMake 配置、示例和 `library/`。 |
| `lyflow-editor-vX.Y.Z.tgz` | `npm pack packages/editor` 的产物：`@lyflow/editor` 的 TypeScript 源码包，版本必须等于产品版本。宿主以 tarball 依赖安装（例如 `"@lyflow/editor": "file:<路径>/lyflow-editor-vX.Y.Z.tgz"`），用自己的打包器编译；不发布到 npm registry。 |
| `LyFlow.Runtime.win-x64.X.Y.Z.nupkg` | 与运行库 ZIP 使用同一份正式暂存内容，统一置于 `runtimes/win-x64/native/`；包含全部 DLL、`lyflow-runtime.json` 和许可证树。根 nuspec 的 repository commit 记录源码修订。诊断构建不生成正式 NuGet 包。 |

算子组合以 `packaging/release.json` 的 `packs` 为唯一来源，当前固定为 `std-pointcloud`、`std-image`、`std-ml`、`glue`、`gap`：`std-*` 随 `LYFLOW_STD_PACKS=1` 编入，其余按名写进 `LYFLOW_PACKS`；不接受环境变量追加 `dts` 或外部算子。gap 包依赖 yaml-cpp，随 vcpkg manifest 固定到 `0.9.0#1`。ONNX Runtime 使用 CPU 版 `1.19.2`。PCL 最低支持版本为 `1.15.1`，发包清单固定到 `1.15.1#1`。core 使用 RelWithDebInfo 与动态 CRT；CLI 使用 Rust Release 和 `--no-default-features`。SDK 从同一份 core 暂存文件生成，避免两套资产实际包含不同引擎。

本版 SDK 的 C++ 客户端与同一发布修订的 Rust 客户端，在 Windows 上将依赖搜索目录限制为指定 DLL 所在目录，不读取 PATH，也不从当前工作目录或 System32 补齐第三方运行库。Windows KnownDLL 与 API Set 系统组件仍由操作系统解析。宿主需把所有非系统依赖部署在 DLL 目录中；缺依赖时应明确失败。GS 必须升级到采用相同产品版本 ABI 的客户端；旧数字 ABI 客户端不再列为兼容对象。

产品版本与 ABI 统一为同一个完整字符串 `X.Y.Z`，例如 `"1.1.0"`。引擎、C++/Rust 客户端、SDK 和包元数据必须逐字一致；补丁版本也不放宽兼容。`abiVersion` 必须是 JSON 字符串，旧数字 `15` 及旧数字 ABI 导出不再支持。DLL 通过 `lyflow_abi_version_string()` 报告 ABI，新客户端缺少该符号或收到不同版本时在任何后续 ABI 调用前拒绝加载。`lyflow-runtime.json` 记录 `engineVersion`、`abiVersion`、`sourceRevision`、架构、构建配置、算子包及版本、实际算子、依赖版本和相对文件清单。最终 DLL 查询结果必须与声明一致。`lyflow_source_revision()` 仅对受控发布组合的干净源码返回构建提交；正式包要求它与当前 HEAD 和元数据 `sourceRevision` 完全相同。脏源码或未知修订返回空字符串，仅允许在显式 `-AllowDirty` 的本地诊断包中使用，并将清单标为 `localValidationOnly`；这种产物不能作为正式发布交付。不使用内容哈希或指纹建立包身份、匹配或校验；Git 提交标识仅用于源码修订。

Rust 客户端继续从同一发布提交引用，不附进 ZIP、不发布 crates.io。Release 提供完整 `rev` 的 Cargo 示例。GS 客户端集成与运行库消费验证由 GS 负责，发包不再绑定独立 GS 消费工程或客户端修订。ABI 不兼容时必须更新客户端，不能宣称运行库可直接替换。SDK 消费方必须声明完整产品版本并使用 `find_package(lyflow X.Y.Z EXACT CONFIG REQUIRED)`。下游应保留已验收运行库及随包 CRT，不能无条件用其他版本覆盖。

## 本地入口

前置工具：Windows x64、PowerShell 7、Visual Studio 2022 C++ 工具集和 Windows SDK、CMake/Ninja、Git、仓库声明的 Rust/Node/pnpm，以及 Python 3.12。Python 门禁依赖使用 `scripts/requirements.txt`。

```powershell
pnpm install --frozen-lockfile
python -m pip install -r scripts/requirements.txt
pnpm run package -OutputDir C:\release\lyflow
```

源码必须干净且有有效 Git 修订；不要在正在编辑的工作树里制作正式产物。正式 tag 构建可加 `-ExpectedTag vX.Y.Z`；脚本校验版本，不自动修改源码版本。输出目录应位于仓外或仓内已忽略的构建目录；默认是 `build/release`。已有同名产物时拒绝覆盖，复验构建须选新输出目录。`-AllowDirty`、`-SkipDependencySetup` 只用于本地诊断，tag 发布禁止使用。

入口准备固定版本依赖、构建 core 与 CLI、运行构建门禁、检查暂存目录并生成 ZIP。vcpkg manifest 固定 baseline；构建不从相邻项目复制依赖。vcpkg 工具和安装树分别位于 `build/release-tools/vcpkg`、`build/release-deps/vcpkg_installed`，二进制缓存位于 `build/release-deps/binary-cache`。ONNX Runtime 位于 `third_party/onnxruntime/onnxruntime-win-x64-1.19.2`。GitHub 分别缓存 vcpkg 工具、vcpkg 二进制缓存和解压后的 ONNX Runtime。工具缓存包含固定 baseline 的 Git 仓库（含 `.git`、ports、versions、scripts、triplets）和 `vcpkg.exe`，排除 `downloads`、`buildtrees`、`packages`、`installed`；键为 `vcpkg-tool-windows-x64-<baseline>-v1`，不包含运行编号或 MSVC 版本，不跨 baseline 恢复。命中后仍核对实际 Git 修订、已跟踪源码状态，以及可执行文件版本是否与 baseline 内声明的工具发布版本一致；未命中时浅拉取固定提交并 bootstrap，修订或工具版本不符、缺失可执行文件时重新准备工具，验证失败即停止。vcpkg 二进制缓存使用 `windows-2022`、x64、完整 MSVC toolset 版本、vcpkg baseline 和发布组合修订 `std-glue-gap-v1` 构成稳定键；同一身份不再按运行编号重复保存。精确键未命中时按 `restore-keys` 退回同 toolset、同 baseline 的最近一份：vcpkg 按每个包的 ABI 哈希取缓存，哈希不符的包自行重编、不会误用，所以调整发布组合时只编新增或变化的包。调整依赖 features、triplet 或发布组合时仍须同步更新缓存身份（当前 `std-glue-gap-v1`），让新组合另存一份。ONNX Runtime 按 Windows x64、CPU 版和 `onnxruntimeVersion` 精确缓存。恢复后仍运行固定依赖版本和必需文件检查；依赖准备与缓存保存在独立的 `deps` job 里完成，`build` job 恢复同一缓存后再执行同一个 `pnpm run package` 完整构建及暂存检查。依赖准备失败不保存不完整缓存，后续软件门禁失败不影响已保存依赖。缺缓存时仍须能从固定输入完整构建。

工作流在成功或失败后保存 `windows-release-build-diagnostics`，保留 7 天；收集构建日志、Cargo 编译/单测日志、CTest JUnit/日志、暂存清单及 DLL 检查的 JSON/日志。最终两个 ZIP、编辑器 tarball、NuGet 包与 `lyflow-runtime.json` 保留 14 天，不生成 ZIP 消费验收报告。

## 正式运行库 NuGet 分发

`LyFlow.Runtime.win-x64` 是下游直接消费的正式运行库包，与下面 vcpkg 编译缓存用途独立。版本跟随完整产品版本；已经存在的版本禁止覆盖，推送发生版本冲突时明确失败。包由固定官方 `nuget.exe 6.14.0` 的 `pack` 命令生成，不重新编译引擎，也不使用内容哈希作为包身份或校验。

`Publish native runtime NuGet` 工作流在 Release 人工正式发布后触发。也可在 Actions 手动输入已正式发布的 tag，为现有 `v1.1.0` 制作首包。两种方式均从当前默认分支检出受信任脚本，只下载选定正式 Release 的运行库 ZIP；拒绝草稿、预发布、元数据/实际 DLL 身份不符及缺少必需算子的资产。独立的 PowerShell C ABI 探针按目标版本核对实际 DLL，不绑定当前 Rust 客户端，因此可以补发先前正式版本；不运行 Release tag 内的源码脚本。普通打包继续使用现有客户端探针作为当前版本构建门禁。ZIP 与 NuGet 包都来自同一份运行库文件；NuGet 不另行编译或拼接依赖。

本机制作并验证包而不上传：

```powershell
./scripts/publish-runtime-nuget.ps1 -Tag v1.1.0 -PackageOnly -OutputDir C:\release\lyflow-nuget
```

去掉 `-PackageOnly` 才会上传。认证依次使用 `LYFLOW_NUGET_TOKEN`、`NUGET_AUTH_TOKEN` 或 `-CredentialFile`（本机默认 `C:\Users\11601\.ssh\github_nuget_auth.txt`，内容为单个令牌）。临时 NuGet 配置只引用环境变量，不保存令牌值，不进入包或 Actions 缓存；失败输出会遮盖令牌。Release API 可使用 `GH_TOKEN`，公开资产本机可匿名下载。CI 用当前仓库的 `github.token`，仅 NuGet 发布 job 获得 `packages: write`；nuspec repository 关联 `liangyuR/lyFlow`。下游仓库需获得包的 Actions 读取授权，本机读取需 `read:packages`。

下游可固定兼容系列后显式选取系列内正式补丁版本，但每次构建必须锁定完整 NuGet 版本及源码修订，并使用同一修订的 Rust 客户端。`1.1.x` 系列选择不放宽运行时完整 ABI/产品版本必须一致的限制。包元数据清单、目录、架构、实际 DLL 版本/修订和算子仍须校验，缓存命中不能替代验证。

独立契约回归：`pwsh -NoProfile -File tests/release/test-runtime-nuget.ps1`，覆盖正式输入、错误版本/ABI/修订/架构、诊断包、旧内容摘要字段、缺算子、异常文件、x86 DLL 和压缩包路径穿越。真实 DLL 检查及首包验证需另外执行上述本机入口。

## 依赖二进制源（GitHub Packages）

Actions 缓存之外，vcpkg 还挂一个常驻的二进制源：`https://nuget.pkg.github.com/liangyuR/index.json`（NuGet）。它不受 Actions 缓存 7 天未用即清、仓库 10 GB 上限的影响。`scripts/use-vcpkg-feed.ps1` 把它接到当前进程的 `VCPKG_BINARY_SOURCES`：本地 `files` 缓存在前，源在后，都没有才编译。`prepare-release-deps.ps1` 在设置了 `LYFLOW_VCPKG_FEED_TOKEN` 时自动调用它；没有令牌时行为与以前相同。

| 使用方 | 令牌 | 模式 |
|---|---|---|
| 发布工作流 `deps` job | 工作流自带 `GITHUB_TOKEN`，job 权限 `packages: write` | `readwrite` |
| 发布工作流 `build` job | `GITHUB_TOKEN`，`packages: read` | `read` |
| 开发机 | classic PAT，`read:packages`（要上传则 `write:packages`）；GitHub Packages 的 NuGet 不认 fine-grained 令牌 | `read`（默认） |

开发机配置：`gh auth refresh -h github.com -s read:packages` 后把 `gh auth token` 存进用户环境变量 `LYFLOW_VCPKG_FEED_TOKEN`；或在 GitHub 设置里新建 classic PAT。任何 vcpkg（包括下游项目自己的 `C:cpkg`）都可以用：`pwsh -File scripts/use-vcpkg-feed.ps1 -VcpkgRoot C:cpkg` 之后在同一进程里 `vcpkg install`。令牌只写进 `build/vcpkg-feed/nuget.config`（已忽略），不进仓库、URL 或日志。

vcpkg 只把本次现编的包推到源里，从 Actions 缓存恢复的包不会再推。给源补全（或换 baseline / toolset 后预热）时，手动运行 `Windows release` 并勾选 `rebuild_deps`：`deps` job 跳过 Actions 依赖缓存，全部重编并推送。

命中要求三元组、vcpkg baseline 与 MSVC 工具集版本都和写入方相同：vcpkg 的包 ABI 哈希包含编译器版本。CI 的 `windows-2022` 与装了更新 Visual Studio 的开发机工具集不同时，开发机下不到 CI 编的包，只会退回本地编译；工具集相同的开发机之间可以用 `write:packages` 令牌和 `-Mode readwrite` 互相共享。

## GitHub 门禁与正式发布

| 触发 | 行为 |
|---|---|
| PR / main push / 手动 `CI` | 仅编译、单元测试、类型与格式检查，不运行完整发包。 |
| 手动 `Windows release` | 可选择实现分支，运行完整门禁，保存 ZIP 和清单 14 天；不创建 Release。 |
| `vX.Y.Z` tag push | tag 必须与源码产品版本一致，且提交位于 main 历史；门禁成功后创建草稿 Release，人工正式发布。 |

完整构建使用 GitHub 托管 `windows-2022`。`deps` job 只安装固定 baseline 的 vcpkg 端口，持有 `packages: write` 以写入依赖二进制源；编译 job 仅有 `contents: read` 与 `packages: read`。发布 job 依赖构建成功，下载两个 ZIP、编辑器 tarball 与清单，再以 `contents: write` 创建草稿并上传；不在持有写权限时编译或运行库代码。

`Windows release` 独立运行，不再由日常 `CI` 调用，也不在 main push 时自动运行。需要发包时手动选择分支，或推送与产品版本匹配的 `vX.Y.Z` tag。

正式发布顺序：先合并经过审查的版本改动并确认日常 CI 成功，再推送匹配版本的 tag；检查草稿资产、ABI、算子组合及源码修订，最后人工发布。GS 仅引用已正式发布的固定 tag 和资产名，不使用 `latest`。签名和自动更新不属于首版。

工作流拒绝任何已存在同版本 Release，包括草稿，不覆盖旧资产。若上传中断，保留草稿供排查，禁止通过重跑覆盖；采用新产品版本重新发包。源码、依赖或交付文件改变，都必须增加产品版本。

## 验收范围与证据

发布门禁覆盖固定依赖、core 单测、完整标准包的 Rust 单测、manifest 自检、headless CLI 构建、暂存 DLL 的 ABI/产品版本/源码修订、x64 架构、算子组合及 packaging/release.json 声明的必需算子、ONNX Runtime 版本与实际模块来源，以及运行库/SDK 布局、许可证和测试模型/调试符号排除。打包时仍生成原有 core 单测要求的合成 ONNX 模型，缺少夹具即失败。

Rust 单测复用本轮 `build/release-core/bin` 的 DLL，使用 `cargo test --locked --release --manifest-path bridge/Cargo.toml --no-default-features --features host --lib --bins`；固定 `LYFLOW_STD_PACKS=1`，执行日常纯平台构建中因缺少标准包而忽略的测试。`host` 启用 IPC/执行层测试而无需桌面资源。CLI 产物仍由无默认特性的 `cargo build --release --bin lyflow` 生成。单测失败保留 Cargo 退出码与日志并停止，不进入暂存及 ZIP 生成。

已删除打包入口中的 ZIP 解压消费验收、独立 runner 验收 job、`package:verify` 入口、固定 GS 客户端消费工程及专用合成场景/损坏资产负向探针。DLL 身份读取由 `scripts/inspect-runtime.ps1` 和本地 Rust 探针完成，不解压 ZIP，不运行外部 C++/Rust 消费场景。SDK 消费工程保留在本地 `pnpm check`，日常 CI 仅编译与单元测试。

本文件描述实现契约，不构成任何一次构建的通过证据。实际通过以对应源码修订的 GitHub 构建运行与随包清单为准；静态检查不代表干净 runner 的完整构建通过。

GlueSight 安装包与静默安装验收由 GS 工作流负责。最终 ZIP 消费、客户图像效果、真实设备、现场驱动、节拍和长期稳定性由下游按需验收，构建成功不能代替这些结果。

正式构建在固定依赖准备通过后显式启用 `LYFLOW_RELEASE_BUILD`，并只允许 Windows x64、RelWithDebInfo、固定的 `release.json` 算子包组合（`core/CMakeLists.txt` 的发布身份与之逐字一致）且无外部包的干净源码构建导出可复用源码修订。普通开发或外部算子构建返回空修订，不能启用跨进程磁盘缓存，也不能冒充正式依赖包。

2026-10-11 的 main 构建 `c625155c` 虽通过编译、单测与 DLL 身份检查，但不含 GlueSight 必需的 `glue.taught_path`，不能作为 GS 示教业务依赖。后续修复移植原 PR #8 的示教胶路算子及其配套测量行为，并在 `packaging/release.json` 声明发布包必需算子；暂存 DLL 检查会拒绝缺少任一必需算子的包。新包是否满足 GS 软件契约仍以实际 DLL 检查和现有测量测试为准，不能用新算子注册或合成图通过替代现场准确率验收。

2026-10-12 起 gap 包并入主发布包（`packaging/release.json` 加 `gap` 与 5 个必需的 gap 算子，vcpkg manifest 加 yaml-cpp），同时发布 `@lyflow/editor` 的 tarball。xyz-gap-inspector 由此可以只钉发布版本与资产校验和，不再以 submodule 钉提交、本地编 core。gap 算子的现场准确率仍由 xyz-gap-inspector 的回放与复核验收，发包门禁只覆盖 gap 包单测与算子注册。

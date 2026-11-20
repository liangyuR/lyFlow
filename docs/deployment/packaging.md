# Windows x64 运行库与公共工具发行

[文档首页](../README.md) · [快速开始](../getting-started.md) · [测试地图](../testing.md)

## 下载与版本

公共工具版本取自 [package.json](../../package.json) 的 `version`，该工具固定使用的引擎版本取自 `lyflowEngineVersion`。每个[引擎版本锁](../../packaging/engine-releases/)记录原生 SDK、runtime ZIP 和 NuGet 原包的公开下载地址、SHA-256、引擎仓及精确修订。Rust 客户端按主、次版本检查 ABI。补丁级工具更新可继续依赖原有的已发布引擎。

| 下载项 | 版本与用途 |
| --- | --- |
| `lyflow-sdk-win-x64-vT.zip` | 完整 SDK，按工具版本 T 命名。包含锁定的原生 SDK 及公共仓编译的 Release CLI，解压后直接运行 `bin/lyflow.exe`。 |
| `lyflow-editor-vT.tgz` | 同版 `@lyflow/editor` TypeScript 源码包。React 宿主按 tarball 依赖安装并编译。共享可视化实现随包提供，React 等 peer 依赖由宿主提供，详见[嵌入指南](../embedding.md)。 |
| `lyflow-runtime.json` | 完整 SDK 内 `bin/lyflow-runtime.json` 的独立副本。包含实际加载的引擎版本 E、ABI、修订、算子及依赖。 |
| `lyflow-native-sdk-win-x64-vE.zip` | 原生 SDK，按引擎版本 E 命名，含 C/C++17 头文件、库、CMake 配置、schema、算子 manifest、Python worker 和示例。 |
| `lyflow-runtime-win-x64-vE.zip` | 供宿主部署的 Windows x64 运行库、依赖、清单与许可证。 |
| `LyFlow.Runtime.win-x64.E.nupkg` | 运行库 NuGet 原包，原生文件位于 `runtimes/win-x64/native/`。GitHub Packages 保留同一份原始字节。 |

工具与新引擎同步发布时，六件资产位于同一个公共 [GitHub Release](https://github.com/liangyuR/lyFlow/releases)。只更新工具时，工具 Release 提供前三件和对应引擎 Release 的链接。旧 Release、tag 和已分发的包可继续按原地址恢复。

## 本地组包与消费验收

Windows x64 下安装 [Rust 和 Node.js 开发环境](../getting-started.md)，使用 PowerShell 7。默认从公开地址匿名恢复 SDK，校验归档 SHA-256、正式构建标记、版本及修订，然后构建 CLI、运行随包示例并打包编辑器。输出目录须为新路径。

```powershell
./scripts/package-windows.ps1 -OutputDir build/tool-release
```

新引擎仍处于暂存阶段时，可传 `-Archive <原生 SDK ZIP>` 校验并使用本地原始资产。需要用固定工具 tag 的代码重组缺失下载时，`-SourceRoot <干净工具检出> -ExpectedTag vT` 核对版本、提交和工作树。`-LockFile` 可显式指定该版本的锁文件。已发布文件保持原字节。

在 x64 MSVC 开发环境下验证完整 SDK 和外部 React 宿主。需能使用 CMake、Ninja、dumpbin、Python、npm 及 Chrome 或 Edge。

```powershell
python -m pip install -r scripts/requirements.txt
./tests/release/test-public-tools.ps1 -AssetsDir build/tool-release
```

验收从归档解压到新目录，核对 CLI 实际加载的 DLL，再运行 P4/P5、Gap/DTS/Glue、原生帧录制回放和 Profile 工作流。外部 C++17 宿主通过 `find_package(lyflow … EXACT)` 链接运行，缺失依赖的案例也在隔离目录验证。P4 公共模型按 SDK 随附清单恢复，可通过 `LYFLOW_P4_ASSETS` 指向已有的校验文件。外部 React 项目安装发行 tarball，使用 TypeScript 和 Vite 生成生产构建；浏览器中通过 HTTP 打开、运行 SDK 内的示例图。客户数据、设备和生产节拍由实际宿主验收。

## 发布步骤

1. 在[引擎仓](https://github.com/liangyuR/lyflow-engine)构建并验证固定版本的三个原包。新增引擎版本时，将它们以原文件名上传至公共仓的 `engine-assets-vE` 暂存草稿，并把允许分发的地址、摘要和私有修订写入公共仓 `packaging/engine-releases/vE.json`。先将版本锁合入默认分支。
2. 为工具版本 T 更新公共仓各包版本、`lyflowEngineVersion` 和 CHANGELOG。手动运行 [Windows release](../../.github/workflows/release-windows.yml) 会从正式引擎 Release 或同版暂存草稿取三件原包，核对受信任锁，在 Windows 上打包和验收，再保存 Actions 产物供检查。
3. 在已合入默认分支的工具提交上推送 `vT` tag。工作流用同一组已验证资产创建正式版本草稿，维护者复核后发布。发生工具补丁更新时，锁定的引擎须已正式发布。原生依赖、算子测试和运行库签核由引擎仓负责。
4. 对于同版引擎发行，Release 发布事件触发 [NuGet 工作流](../../.github/workflows/publish-runtime-nuget.yml)。它从公共默认分支取得受信任锁，验证正式发布状态、原包摘要、nuspec 包 ID、版本、引擎仓及修订，再将同一文件推送到 [GitHub Packages](https://github.com/liangyuR?tab=packages)。已存在的 NuGet 版本拒绝再次推送。只更新工具时，宿主继续还原原有 NuGet 版本。

本地仅验证已发布的 NuGet 原包可执行下列命令，将 `v2.0.3` 换成目标引擎版本。

```powershell
./scripts/publish-runtime-nuget.ps1 -Tag v2.0.3 -PackageOnly -OutputDir build/verified-runtime
```

[原生引擎仓](https://github.com/liangyuR/lyflow-engine)维护原生构建输入、发布组合、C ABI、运行库探针与原生 SDK 验收。历史单仓发行的构建步骤保存在对应的工具 tag 中。

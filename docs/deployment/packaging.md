# Windows x64 运行库与公共工具发行

[文档首页](../README.md) · [快速开始](../getting-started.md) · [测试地图](../testing.md)

## 下载与版本

公共工具版本取自 [package.json](../../package.json) 的 `version`，该工具固定使用的引擎版本取自 `lyflowEngineVersion`。每个[引擎版本锁](../../packaging/engine-releases/)记录原生 SDK、runtime ZIP 和 NuGet 原包的公开下载地址、SHA-256、引擎仓及精确修订。Rust 客户端按主、次版本检查 ABI。补丁级工具更新可继续依赖原有的已发布引擎。

| 下载项 | 版本与用途 |
| --- | --- |
| `lyflow-sdk-win-x64-vT.zip` | 完整 SDK，按工具版本 T 命名。包含锁定的原生 SDK 及公共仓编译的 Release CLI，解压后直接运行 `bin/lyflow.exe`。 |
| `lyflow-editor-vT.tgz` | 同版 `@lyflow/editor` TypeScript 源码包。React 宿主按 tarball 依赖安装并编译。共享可视化实现随包提供，React 等 peer 依赖由宿主提供。2.0.3 的浏览器宿主也须显式安装 `@tauri-apps/api@^2`，使 TypeScript 和打包器能解析编辑器入口导出的传输模块。详见[嵌入指南](../embedding.md)。 |
| `lyflow-runtime.json` | 完整 SDK 内 `bin/lyflow-runtime.json` 的独立副本。包含实际加载的引擎版本 E、ABI、修订、算子及依赖。 |
| `lyflow-native-sdk-win-x64-vE.zip` | 原生 SDK，按引擎版本 E 命名，含 C/C++17 头文件、库、CMake 配置、schema、算子 manifest、Python worker 和示例。 |
| `lyflow-runtime-win-x64-vE.zip` | 供宿主部署的 Windows x64 运行库、依赖、清单与许可证。 |
| `LyFlow.Runtime.win-x64.E.nupkg` | 运行库 NuGet 原包，原生文件位于 `runtimes/win-x64/native/`。GitHub Packages 保留同一份原始字节。 |

工具与新引擎同步发布时，六件资产位于同一个公共 [GitHub Release](https://github.com/liangyuR/lyFlow/releases)。只更新工具时，工具 Release 提供前三件和对应引擎 Release 的链接。旧 Release、tag 和已分发的包可继续按原地址恢复。

## 本地组包与消费验收

Windows x64 下安装 [Rust 和 Node.js 开发环境](../getting-started.md)，使用 PowerShell 7。默认从公开地址匿名恢复 SDK，校验归档 SHA-256、正式构建标记、版本及修订，然后构建 CLI、运行随包示例并打包编辑器。输出目录须为新路径。新组装的 SDK ZIP 使用固定历史时间，跨时区直接解压后也可构建 CMake 示例。恢复早期原生包时，解压脚本会将晚于本机当前时刻的文件时间调至当前时刻。

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

1. 在[引擎仓](https://github.com/liangyuR/lyflow-engine)构建并验证三个原包。新引擎 E 获准分发后，先在公共仓创建独立的 `engine-assets-vE` Release，将三个原始文件以审核通过的名称上传到草稿。该 tag 可指向公共仓当前的默认分支提交。检查草稿后以正式、非预发布、`--latest=false` 的方式公开。公开引擎资产不触发工具发包或 NuGet 推送。
2. 为 E 添加 `packaging/engine-releases/vE.json`。三个 URL 填写上一步已公开的永久资产地址。已有 2.0.3 保留原来的 `v2.0.3` 地址。新锁和 `lyflowEngineVersion` 可以在同一工具 PR 中提交。常态 CI 用提交内的锁匿名下载 SDK，核验公开 Release 的正式状态、资产名称与 URL，再验证 SDK 摘要和引擎身份。已复核的锁合入默认分支后，即可发布工具。
3. 更新工具版本 T 与 CHANGELOG，手动运行 [Windows release](../../.github/workflows/release-windows.yml) 检查资产。工作流读取受信任的默认分支锁，从公开地址下载三个原包、核对摘要，在 Windows 上组装完整 SDK 与编辑器，执行消费验收并保存 Actions 产物。推送指向默认分支历史的 `vT` tag 时，工作流创建工具 Release 草稿。若 T 等于 E，草稿同时附上三个引擎原包的原始字节。若工具继续沿用另一引擎版本，正文链接到该引擎的实际 Release。维护者复核后正式发布。
4. 正式工具 Release 的发布事件触发 [NuGet 工作流](../../.github/workflows/publish-runtime-nuget.yml)。它从工具 tag 的元数据读取引擎 E，从默认分支取得对应的受信任锁，核验公开状态、原包摘要、nuspec 包 ID、版本、引擎仓及修订，再向 [GitHub Packages](https://github.com/liangyuR?tab=packages) 发布 E 的原包。工具补丁沿用已发布的 E 时，工作流下载已注册的 nupkg，核对锁内摘要后结束；同步发行的同版重复推送会报错。

本地仅验证已发布的 NuGet 原包可执行下列命令，将 `v2.0.3` 换成需要验证的工具 Release tag。

```powershell
./scripts/publish-runtime-nuget.ps1 -Tag v2.0.3 -PackageOnly -OutputDir build/verified-runtime
```

[原生引擎仓](https://github.com/liangyuR/lyflow-engine)维护原生构建输入、发布组合、C ABI、运行库探针与原生 SDK 验收。历史单仓发行的构建步骤保存在对应的工具 tag 中。

Actions 查询 GitHub Release 元数据时使用当前公共仓的读取权限。所有引擎文件均从锁内的公开下载地址匿名获取。

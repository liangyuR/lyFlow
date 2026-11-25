# Windows 快速开始

公共工具适用于 Windows x64。构建需要 PowerShell 7、Git、Visual Studio 的“使用 C++ 的桌面开发”工具集及 Windows SDK、Rust MSVC 工具链、Node（见 `.nvmrc`）和根 `package.json` 指定的 pnpm。桌面使用 WebView2 Runtime。Python 3.12 与 `scripts/requirements.txt` 用于测试和运行 Python 节点。

## 恢复 SDK，运行第一张图

从公共仓的新检出执行：

```powershell
pnpm install --frozen-lockfile
pnpm sdk:restore
pnpm cli:build
& ./bridge/target/release/lyflow.exe info
& ./bridge/target/release/lyflow.exe manifest --check
& ./bridge/target/release/lyflow.exe run build/native-sdk/examples/p2-expression.lyflow.json --summary
```

最后一行 JSON 的 `outputs.mean` 应报告 13 mm。恢复脚本从公共 Release 下载 SDK，核对版本锁的 SHA-256 及包内引擎修订，再放到 `build/native-sdk`。Cargo、`pnpm dev`、`pnpm check` 与 `pnpm e2e` 未设置 `LYFLOW_NATIVE_SDK` 时都使用这个目录。版本锁升级后先删除 `build/native-sdk` 再执行 `pnpm sdk:restore`；要并存多个版本时，用 `./scripts/restore-native-sdk.ps1 -Destination <新目录>` 恢复并把 `LYFLOW_NATIVE_SDK` 设为该绝对路径。

## 在桌面运行

```powershell
pnpm dev
```

在桌面打开同一张 `p2-expression.lyflow.json`，按 F5，然后查看 `mean` 输出。CLI 和桌面加载各自 exe 目录的运行库。日常编辑器开发使用 Vite 热更新。原生开发者可用调试构建，并把 `LYFLOW_WATCH_DLL` 设为独立引擎构建输出 DLL 的绝对路径来监视重编。修改引擎、算子或 Python worker 请使用[引擎仓](https://github.com/liangyuR/lyflow-engine)。

## 浏览器宿主与测试

构建 CLI 后，在一个终端启动开发用 HTTP 服务，在另一个终端启动 React 示例：

```powershell
node packages/editor/test-server/server.mjs --port 8787 --root . --cli bridge/target/release/lyflow.exe
```

```powershell
pnpm host:dev
```

运行本地检查前安装验证依赖：

```powershell
python -m pip install -r scripts/requirements.txt
pnpm check
```

命令、数据和验收边界见[测试地图](testing.md)。C++ 宿主可从正式 SDK 使用 `find_package(lyflow … EXACT)`，下游运行库和 React 编辑器的安装方式见[交付指南](deployment/packaging.md)。

# Windows x64 双仓发行消费验收

2026 年 11 月 20 日，在隔离目录验证了 [2.0.3 正式发行](https://github.com/liangyuR/lyFlow/releases/tag/v2.0.3)的原生 SDK、运行库、NuGet 和公共宿主。桌面和 CLI 使用本次公共标签的源码，以正式 SDK 构建为 Release 可执行文件。

| 入口 | 实际操作和结果 |
| --- | --- |
| 原生 SDK | 从公共版本锁的 URL 匿名下载。恢复脚本核对归档摘要、引擎版本、源码修订与正式构建标记。SDK 报告引擎和 ABI 均为 2.0.3。在独立目录通过 `find_package(lyflow 2.0.3 EXACT)` 配置随包的 `examples/public-kernels`，MSVC x64 编译链接并通过 CTest。 |
| NuGet | 从 GitHub Packages 无缓存恢复精确版本 `LyFlow.Runtime.win-x64 2.0.3`。包摘要与版本锁一致，直接加载包中的 DLL 得到引擎及 ABI 2.0.3。 |
| 桌面、CLI 与 MCP | SDK 自带的 `examples/p2-expression.lyflow.json` 在三个入口均得到 `mean = 13 mm`。WebView2 通过 Tauri 执行并读回命名输出，Windows 进程模块显示 DLL 来自新部署目录。CLI 通过 `info` 核对实际 DLL 路径；MCP 通过标准输入输出连接，HTTP 测试宿主调用同一 CLI，先验证两端身份，再用 `run_graph` 和 `summarize_output` 读取结果。 |
| 加载失败 | 独立目录中只有 CLI 时，`info` 退出码为 2 并报告缺失 DLL 的完整路径。同目录放入完整的 1.1.6 运行库后，退出码为 2，诊断列出客户端 2.0.3、ABI 1.1.6 和引擎 1.1.6。 |

## xyz-gap-inspector 升级与回退

使用下游仓库的隔离检出与其锁定的 Rust 客户端。下游恢复脚本将 2.0.3 下载并校验到独立缓存，上一版使用正式 Release 的 2.0.2 运行库并核对发行摘要。每次以新的宿主进程启动，沿下游支持的 `LYFLOW_CORE_DLL` 加载路径，通过实际的 `LyFlowMeasurer` 将一对归档 Profile 注入同一测点图。

| 顺序 | 宿主报告的 DLL 和引擎 | 模板分支的点位结果 |
| --- | --- | --- |
| 升级 | `v2.0.3/lyflow_core.dll`，2.0.3 | 间隙 3.29 mm，面差 4.14 mm |
| 回退 | `v2.0.2/lyflow_core.dll`，2.0.2 | 间隙 3.29 mm，面差 4.14 mm |
| 再升级 | `v2.0.3/lyflow_core.dll`，2.0.3 | 间隙 3.29 mm，面差 4.14 mm |

三次运行均报告模板定位、裁剪已应用，间隙与面差量测成功。验收覆盖宿主测量链的离线回放；安装程序、现场采集、PLC 和生产节拍仍由业务宿主独立验收。

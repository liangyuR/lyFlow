# 私有引擎与公共工具链的交付

## 两个仓库各自交付什么

| 仓库 | 维护范围 | 产物 |
| --- | --- | --- |
| [lyflow-engine](https://github.com/liangyuR/lyflow-engine)（私有） | 原生引擎、算法与算子注册、C ABI、公共 C++ 内核和头文件、Python worker、原生依赖、数值与 SDK 测试 | Windows x64 runtime ZIP、原生 SDK ZIP、运行库 NuGet、运行库清单与原包摘要 |
| [lyFlow](https://github.com/liangyuR/lyFlow)（公共） | Rust 客户端、CLI、MCP、Tauri、React 编辑器、公共示例、跨进程与宿主消费验收 | 含 Release CLI 的完整 SDK ZIP、编辑器 tarball，以及经版本锁核验的原始引擎资产 |

工具仓默认从 `package.json` 的 `lyflowEngineVersion` 选择引擎。对应锁记录完整引擎版本、引擎修订、下载地址和 SHA-256。工具与引擎各自保留源码标识；公共工具的独立补丁可继续使用已发布引擎。SDK 的恢复入口、发布顺序和资产名见[发包说明](packaging.md)。

## 首次双仓发行后的遗留计划

| 工作项 | 实现与验收入口 |
| --- | --- |
| 公共工具下载 | [2.0.3 正式 Release](https://github.com/liangyuR/lyFlow/releases/tag/v2.0.3) 提供完整 SDK 和编辑器 tarball。工具组装直接使用锁定的 SDK，交付检查从 ZIP 运行 CLI 并在仓库外安装编辑器、打开和运行图。 |
| 日常 CI 与原生源码归属 | 公共 Windows 作业匿名恢复 SDK，运行 Rust、CLI、MCP 与 WebView2。平台、算子算法和原生构建门禁归引擎仓；公共仓保留图、包说明、宿主示例与工具侧契约检查。两侧入口见[测试地图](../testing.md)。 |
| 正式渠道消费与下游回退 | 经公共 Release、GitHub Packages、外部 C++17 工程和真实桌面验证；代表下游宿主按正式运行库往返切换，逐次核对加载身份和输出。详见[Windows 消费验收](windows-consumption-2.0.3.md)。 |
| 开发指南 | [快速开始](../getting-started.md)从公开 SDK 运行第一张图，[路线图](../roadmap.md)列出已交付的包边界和需要独立验收的业务范围。 |

## 版本与回退

下游按完整版本恢复原包。SDK 文件与可信摘要、声明的引擎修订有差异时，恢复脚本终止。公共工具验收失败时，上一个正式版本仍可下载。宿主回退应同时核对实际 DLL 路径、版本与代表图输出。已发布文件保持原字节，公共工具或引擎修复使用新版本发行。

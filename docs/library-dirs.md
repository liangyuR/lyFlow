# 库算子目录的设置

> 2026-09-29。库算子本身见 [ADR-0010](adr/0010-subgraph-by-expansion.md)。

## 目录从哪来

完整列表按这个顺序，后面与前面重复的去掉（Windows 上不分大小写、不分斜杠方向）：

1. **默认目录** `<app data>/library/`（`%APPDATA%\com.lyflow.app\library`）——「保存到库」写这里，永远在第一个。
2. **设置里加的** —— 工具栏「库 ▾」里增删，存在 `<app data>/library-dirs.json`：`{ "extraDirs": ["D:/libs", …] }`。
3. **环境变量** `LYFLOW_LIBRARY_DIRS`（分号分隔）—— 面板里标出来，只读。

**app 与 CLI 读同一份**（`bridge/src/library_settings.rs`）：两边各算一份迟早漂开，就会出现「界面里能跑、CLI 里报缺算子」。
嵌入宿主经 `HostConfig.library_dirs` 整个指定了目录时，以宿主为准，面板只读并写明「目录由宿主配置」。

## 界面

工具栏「库 ▾」：列出三类目录；粘贴路径回车（或「浏览…」，宿主给了选目录对话框时才有）添加，× 去掉（目录本身不删）；
改了**当场保存并重扫**，manifest 跟着换。「重扫」照旧在里面 —— 库文件是手工放进去的，不重启就生效。

新加的目录**下次启动**才会被 watcher 盯着自动重扫；在那之前改了库文件点「重扫」。

## 接口

| 层 | 读 | 写 |
|---|---|---|
| Tauri 命令 | `get_library_settings` → `{ defaultDir, extraDirs, envDirs, editable }` | `set_library_dirs(extraDirs)` → 写设置、重扫，回 `{ status, manifest }`（同 `refresh_library`） |
| Transport | `getLibrarySettings()` | `setLibraryDirs(extraDirs)` |
| HTTP | `GET /lyflow/library/settings` | `POST /lyflow/library/dirs`（桩服务器没有库目录：只读的空设置、写回 501） |

## 测试

- `bridge/src/library_settings.rs`：读写往返、规整（去空白 / 去空串 / 去重）、合并顺序、坏文件当空。
- e2e `m4.mjs` §1 库算子：面板里添加目录 → manifest 里出现那个目录的库算子、CLI 的 `lyflow manifest` 也有、× 去掉就没了；设置写在真的 app data 里，收尾还原。

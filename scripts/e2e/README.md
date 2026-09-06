# e2e —— CDP 验收

用 CDP 驱动**真实运行的 Tauri app**，覆盖 [docs/m2-plan.md](../../docs/m2-plan.md) §11
里所有标着「CDP」的验收项，外加中文路径那一条。逐条结果见
[docs/m2-acceptance.md](../../docs/m2-acceptance.md)。

```
cdp.mjs      极简 CDP 客户端
harness.mjs  起 app、连 CDP、记断言、收尾
page.mjs     页面侧的公共动作（搭图、按键、真鼠标拖拽、等一次运行）
run.mjs      M2 的分组 + main
m3.mjs       M3 的分组（缓存、静音、迁移、连线手感、布局、面板、3D）
m4.mjs       M4 的分组（子图、库算子、live preview、大图性能）
gap.mjs      gap 领域包的分组（量测输出、真实 gap 图、模型 gap 图）
phase_a.mjs  阶段 A 的分组（惰性分支半透明、plan_extended、图级输出与「标为输出」UI）
http.mjs     e2e:http —— Node 桩服务器 + 系统 Chrome + examples/host-react
```

CLI（m4-plan §3）不在这里：它没有界面，验收走 `cargo test`
（`bridge/src/cli.rs` 的 `mod tests`），理由见 docs/m4-acceptance.md。

## 为什么是 CDP，不是前端单元测试

CLAUDE.md 规定不写 UI 单元测试。而 M1 的经验是——真正会咬人的 bug
（d3-zoom 吞掉双击、MiniMap 不画节点、选中状态渲染死循环）没有一个是单元测试
能发现的，它们全都要求「真实的浏览器 + 真实的事件 + 真实的后端」。

同理不用 puppeteer / playwright：它们要下载自己的浏览器，而我们要驱动的是
**Tauri 里那个 WebView2**，不是另开一个 Chrome。真正需要的能力只有三件：
找到 target、开 WebSocket、发 `Runtime.evaluate` —— 一百来行，不值得为它引一棵
几百兆的依赖树，也不值得让验收依赖一次网络下载。Node 22+ 自带 WebSocket 与
fetch，所以 `cdp.mjs` 零依赖。

## 跑法

```bash
pnpm e2e           # 自己起 tauri dev，跑完自己收尾
pnpm e2e:packaged  # 同一套断言，跑 tauri build 产物在干净目录里的拷贝
```

调试脚本本身时，另开一个窗口跑

```bash
WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS="--remote-debugging-port=9222" pnpm tauri dev
```

然后 `LYFLOW_E2E_ATTACH=1 pnpm e2e`，省掉每次两分钟的重编。

`launchApp` 因此有三种模式：

| 模式 | 行为 |
|---|---|
| 默认 | 起 `tauri dev` |
| `packagedExe` | 起一个已经打包好的 exe（干净目录验收） |
| `LYFLOW_E2E_ATTACH=1` | 连到已经开着的实例 |

`WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` 是 WebView2 官方的注入口，必须在
**进程启动前**设进环境变量 —— WebView2 只在创建环境时读一次。

## `pnpm e2e:http`

另一条线：验的是 `@lyflow/editor` + `HttpTransport` 在**普通浏览器**里的样子。
Tauri 那条一条断言都不共享后端，但共享 `cdp.mjs` / `harness.mjs` / `page.mjs`。

```
node scripts/e2e/http.mjs
```

它自己拉起三样东西，跑完全部收掉：

1. `packages/editor/test-server/server.mjs` —— [docs/http-transport.md](../../docs/http-transport.md)
   的最小实现，每个请求起一次 `lyflow` CLI。工作区是一个临时目录，鉴权 token 每次随机。
2. `examples/host-react` 的 vite dev server（5174），
   `VITE_LYFLOW_API` / `VITE_LYFLOW_TOKEN` 经环境变量注进去 —— **不进 URL**。
3. 系统装的 Chrome 或 Edge，带 `--remote-debugging-port` 与一个一次性 user-data-dir。

**前置条件只有一个**：`bridge/target/debug/lyflow.exe` 存在。没有就先

```
cargo build --manifest-path bridge/Cargo.toml --bin lyflow --no-default-features
```

（`pnpm check` 会顺手建好这一份。）

可调的环境变量：`LYFLOW_HTTP_PORT`（默认 8788）、`LYFLOW_HOST_PORT`（5174）、
`LYFLOW_HTTP_CDP_PORT`（9333）、`LYFLOW_BROWSER`（浏览器可执行文件）、
`LYFLOW_E2E_HEADLESS=1`（无头，3D 那条要 SwiftShader 才过）。

宿主侧的窗口桥是 `examples/host-react/src/bridge.ts`，形状与 app 的 devbridge 一样
但只有 HTTP 子集用得到的那些。

## 干净目录验收（`stagePackagedApp`）

把 `tauri build` 的产物复刻成一个干净目录里的安装结果：拷 exe + 同目录的全部
DLL —— 这正是两个安装包往 `$INSTDIR` 放的东西（NSIS 的 `SetOutPath $INSTDIR`，
WiX 的 `INSTALLDIR`）。目的是验证「DLL 随包」和「从 exe 同目录加载」这条路径，
而不是验证安装程序本身。

注意这**不等于**在一台干净机器上验证：这台机器有 MSVC 和 vcpkg，漏打包的 DLL
仍然可能被系统从别处找到。

## 踩过的坑

- **等运行结束之前先记下当前的 runId**（`runAndWait`）。连续跑第二次时，
  `runStatus` 在按下 F5 的那一刻还是上一次的 `'ok'`，只等「不是 running」会
  立刻返回上一次的快照，于是断言全部对着旧结果做。症状是「画布上明明是红的，
  快照里却是 done」。
- **读 3D 视图点数之前先等 `.viewer[data-node=<id>]` 切过去**
  （`selectAndReadViewer`），否则读到的是上一个节点残留的计数，得到一个
  「点数 > 0」的假绿。
- **`cdp.eval` 的 `awaitPromise` 默认开着。** 忘了开拿到的是 `{}`
  （一个 Promise 的 JSON 形态），断言会以一种极其费解的方式失败。
- **搭图走 store 的语义化动作，不直接塞 doc。** 塞一份构造好的 doc 会跳过
  `addNode` / `connect` 里的校验与 id 分配，验的就不是真实代码路径了。

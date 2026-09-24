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
peek.mjs     连线内容查看器的分组（双击开窗、四种视图、快照锁、生命周期、边的右键菜单）
phase_a.mjs  阶段 A 的分组（惰性分支半透明、plan_extended、图级输出与「标为输出」UI）
m8b.mjs      M8b 的分组（空白画布拼测点：自动连线、片段、2D 拖框、实时校验、Bundle 的 Edge Peek）
m8c.mjs      M8c 的分组（三模板的图：2D 视图按槽切换、拖一个槽只改它、复制到其它槽、标签不遮挡、按槽的诊断）
motion.mjs   动效的分组（docs/motion-plan.md §3 验收 2–9：进场、删除残影、端点对齐、连线生长、流动、状态闪光、hover、关动效）
noderun.mjs  「只运行此节点」的分组（docs/node-run-plan.md §4 验收 7–12：按钮位置与真鼠标、只重算一个节点、上游不齐的预判与兜底、停止与抢占、hover / 关动效 / 端点对齐、右键菜单）
http.mjs     e2e:http —— Node 桩服务器 + 系统 Chrome + examples/host-react
```

CLI（m4-plan §3、m5-plan §1）不在这里：它没有界面，验收走 `cargo test`
（`bridge/src/cli.rs` 与 `bridge/src/eval.rs` 的 `mod tests`），理由见 docs/m4-acceptance.md。
`lyflow eval` 的批量评估也归这一档 —— 值路径解析、留出与分组统计、glob 生成样本
都是纯逻辑，对着手算的数字断言比开浏览器便宜得多
（[ADR-0020](../../docs/adr/0020-eval-and-perturb-as-cli.md)）。

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
- **HTML5 拖放（面板 → 画布）用 DragEvent + 一个真的 `DataTransfer` 驱动**（`m8b.mjs` 的
  `dropFromPalette`）：CDP 的鼠标事件不会触发 HTML5 拖放。面板行自己的 `dragstart` 把 MIME 写进去，
  落点处 `elementFromPoint` 拿到的元素收 `dragover` / `drop`，走的是画布真实的 onDrop。
  2D 拖框、连线拖拽这类指针手势照旧用真实鼠标（`dragMouse`）。
- **M8b 的最终画布截图**：设 `LYFLOW_E2E_SCREENSHOT=docs/m8b-canvas.png`（相对仓库根）时，
  验收 7 那一组跑完用 `Page.captureScreenshot` 截整个窗口写到那里；不设就不写，免得每跑一遍都改动仓库文件。
  M8c 同理：`LYFLOW_E2E_M8C_SHOT=docs/m8c-slot2.png` 时，验收 12 切到槽 2 那一刻只截 3D 视图那一块（`clip`）。
- **动效会让「刚改完就读」读到半路上的值**（`motion.mjs`、docs/motion-plan.md）。节点刚加上时在播
  200 ms 的进场淡入，stale / 静音 / 未被需要的透明度变化也有过渡：读计算后的 `opacity` 之前先等
  `el.getAnimations()` 里有限的那些播完再过两帧（`phase_a.mjs` 的 `opacityOf`；running 的呼吸光是
  无限循环，别等它）。位置不受影响 —— 进场只动 opacity，自动布局的过渡只在按 Ctrl+L / 右键「整理」
  时才有，`placeAtScreen` 直调 `applyLayout` 一步到位。
- **动效的断言靠标记属性 + MutationObserver。** `data-entering`（节点进场）、`data-growing`（连线生长）、
  `data-flash`（done / error 闪光）只在动画期间挂着，事后查 DOM 什么都看不到；`motion.mjs` 在页面里
  装一个观察器，记下它们出现过几次。关动效用 `Emulation.setEmulatedMedia` 设 `prefers-reduced-motion`，
  宿主的 `animations={false}` 在 `e2e:http` 里点宿主栏上的「动效」开关。
- **单节点运行的断言要看事件，不看节点表**（`noderun.mjs`）。这种运行不清空节点表、计划里其余节点的
  node_state 也不落进 store（验收 8：它们的状态与耗时不变），所以「计划里有谁、isolate 是什么」只能从
  `run_started` 读：分组在页面里另挂一个 `transport.onExecutionEvent` 记下 run_started / run_finished。
  「只有 b 变了」看 `window.__lyflow.transitions` —— 它来自事件，被忽略的那些节点一条都不会有。
- **要一个跑得够久的节点**（验收 10 / 11 的停止、抢占、进度环）：三百万点过一道 0.0006 的体素栅格，
  本机约 1.7 s。等「按钮进入 running」要在页面里逐帧看（`waitButton`），从 Node 侧轮询会错过。
- **同名的输入、输出端口 testid 相同**（voxel 的 `cloud` 进 `cloud` 出都是 `port-<id>-cloud`）。按侧别挑要加
  `.node-port--input` / `.node-port--output`，否则 `querySelector` 拿到的永远是输入那一个。
- **搭图走 store 的语义化动作，不直接塞 doc。** 塞一份构造好的 doc 会跳过
  `addNode` / `connect` 里的校验与 id 分配，验的就不是真实代码路径了。

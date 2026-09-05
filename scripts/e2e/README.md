# e2e —— CDP 验收

用 CDP 驱动**真实运行的 Tauri app**，覆盖 [docs/m2-plan.md](../../docs/m2-plan.md) §11
里所有标着「CDP」的验收项，外加中文路径那一条。逐条结果见
[docs/m2-acceptance.md](../../docs/m2-acceptance.md)。

```
cdp.mjs      极简 CDP 客户端
harness.mjs  起 app、连 CDP、记断言、收尾
run.mjs      各验收分组 + main
```

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

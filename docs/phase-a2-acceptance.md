# 验收：阶段 A 的 A2（前端拆包与 HttpTransport）

对着 [phase-a-plan.md](phase-a-plan.md) 的「A2 验收」逐条走：**怎么跑 + 实际输出 + 通过/未通过/未验证**。
决定与理由见 [ADR-0018](adr/0018-editor-as-package.md)、[docs/http-transport.md](http-transport.md)、
[`packages/editor/README.md`](../packages/editor/README.md)。

环境：Windows 11、MSVC 14.51、CMake + Ninja、vcpkg `C:\vcpkg`（PCL 1.15.1、yaml-cpp 0.9.0）、
onnxruntime 1.19.2、Node 24.16.0、pnpm 11.1.3、Chrome 稳定版、
LyFlow `main` @ `56eebba`（A1）之上。
gap 数据 `C:\Users\11601\OneDrive\Documents\DTS\tianmu_0904`（39 个样本 / 12 份 StandardGap.yml），
基线 `%TEMP%\lyflow-gap-baseline` 与 `%TEMP%\lyflow-gap-baseline-model`，
模型 `C:\Users\11601\OneDrive\Documents\DTS\models\v12s0.onnx`。

## 结论

| # | 验收项 | 状态 |
|---|---|---|
| 1 | `pnpm check` 全绿；`pnpm e2e` 全绿（Tauri 壳行为不变） | ✅ 通过（check 全链路绿；e2e **332 / 332**，基线 323 只增不减） |
| 2 | `pnpm e2e:http` 全绿：HttpTransport 下打开图、改参数、运行、看 3D、取输出 | ✅ 通过（**29 / 29**） |
| 3 | `@lyflow/editor` 能被最小 Vite + React 宿主装上并渲染，宿主的 React 实例被复用 | ✅ 通过（构建期去重插件 + 运行期 hook 探针，两条都绿） |
| 4 | 文档：`packages/editor/README.md`、`docs/http-transport.md`、ADR-0018 | ✅ 通过（另加 `docs/architecture.md` 职责表、`app/README.md`、`scripts/e2e/README.md`） |
| — | A2-5 的 UI 项（`plan_extended`、`not_demanded`、Error 端口色、「标为输出」+ Inspector 列表） | ✅ 通过（前三项 A1 已做，本次补齐后两项，e2e 多九条断言） |
| — | `pnpm check:gap` 全绿 | ✅ 通过 |

---

## 逐条

### 1. `pnpm check` 与 `pnpm e2e`

```powershell
pnpm check
$env:LYFLOW_PACKS="gap"; node scripts/e2e/run.mjs
```

`pnpm check` 末尾打印 **全链路绿**。这一轮比 A1 多两步：

- `pnpm --filter "@lyflow/editor" typecheck` —— 包单独过一遍 strict tsc。
  app 那一遍是顺着 import 走的，只有宿主才会用到的入口（比如 `HttpTransport`）漏在外面。
- `pnpm --filter lyflow-host-react build` —— 宿主示例构建，连带跑重复依赖检查（见第 3 条）。

`pnpm e2e`：**332 / 332 全绿**，包括「跑完全程没有控制台报错」。
基线是 A1 的 323；多出来的九条全在 `phase_a.mjs` 的图级输出分组里，是 A2-5 新加的
（右键菜单里有「标为输出」、名字取自端口名、指向的是刚才那个端口、Inspector 列出了它、
列表里写明来自哪个端口、列表里能取消、列表里补上了输出类型、取消后 doc 上没有了、列表跟着收起来）。
**一条旧断言都没有改动或删除。**

Tauri 壳的行为没变：快捷键表、devbridge 的形状、窗口标题的格式、
浏览器模式的「静态快照」标记全部照旧。壳里现在只剩 `main.tsx` / `dialogs.ts` /
`title.ts` / `devbridge.ts` / `shell.css` 五个文件。

`pnpm check:gap` 同样全绿（导入器逐节点等价 + 三条 A/B 39/39），A2 一行 C++ 都没碰。

#### 中途踩到的两件事

第一轮 `pnpm e2e` 停在 `Ctrl+E` 与 `?` 上：快捷键从 `window` 挪到编辑器根元素之后，
焦点掉回 `document.body` 时按键谁都收不到。修法见 ADR-0018 的第五条
（根元素可聚焦 + 只接管「无主」按键的兜底）。

第二轮的两条失败是我自己造成的：跑到一半时改了 `packages/editor/src/store/graph.ts`，
Vite HMR 把页面整个重载了，`window.__lyflow` 当场消失。第三轮不动源码，332/332。
**结论：e2e 跑的时候别碰前端源码。**

### 2. `pnpm e2e:http`

```powershell
node scripts/e2e/http.mjs
```

前置条件只有一个：`bridge/target/debug/lyflow.exe` 存在（`pnpm check` 会建好）。
脚本自己拉起桩服务器（随机 token、临时工作区）、`examples/host-react` 的 vite dev、
系统 Chrome（一次性 user-data-dir），跑完全收掉。实测输出：

```
工作区 C:\Users\11601\AppData\Local\Temp\lyflow-http-e2e-emsLXm
浏览器 C:\Program Files\Google\Chrome\Application\chrome.exe（CDP 端口 9333）

── 宿主集成：@lyflow/editor + HttpTransport                      8 ✓
── HttpTransport：打开图、改参数、运行、3D、图级输出              15 ✓
── HttpTransport：搭图 + 快捷键运行 + 坏参数                       4 ✓
── 控制台                                                          1 ✓

29/29 项通过，全绿
```

覆盖到的子集，逐条对着 A2-4 的「编辑与运行子集」：

| 要求 | 断言 |
|---|---|
| 打开图 | `transport.loadGraph` 拿回两节点一条边，`outputs` 声明也回来了 |
| 改参数 | `setParam` → `transport.saveGraph` → **读磁盘上的文件确认真的变了** |
| 运行 | `runGraph` + WebSocket 事件 → `run_finished: ok`，两个节点都 `done`，点数 24000，voxel 确实降了采样 |
| 看 3D | 选中 voxel，等 `.viewer[data-node]` 切过去，`canvas` 在且点数 > 0 —— 走的是二进制点云那条路 |
| 取图级输出 | `getRunOutputs` 按名字给出 `thinned`，node/port/type 都对，元素数与节点报的一致 |
| 快捷键 | 真按 F5（CDP 的 Input 域）触发一次运行 |
| 校验 | 坏参数被 `POST /lyflow/validate` 逮到，诊断带 `nodeId` |

3D 那条是端到端的：桩服务器起 `lyflow dump --format ascii` 写 PCD，
在 Node 里解析成 ADR-0006 的二进制布局回给浏览器，前端的 `decodeCloud` 读它。
magic / pointCount / totalPoints / flags 四个字段都必须对，错一个 three.js 就画不出来。

### 3. 宿主复用同一个 React 实例

两条独立的证据，都在门禁里：

**构建期**（`examples/host-react/vite.config.ts` 的 `noDuplicatePeers` 插件）：
扫打包结果的 module id，`react` / `react-dom` / `@xyflow/react` / `three` / `zustand`
任何一个出现两个 `node_modules/<name>/` 根就让构建失败。
每次 `pnpm check` 都跑。实测通过（243 modules transformed，无告警）。

> 写这个插件时先踩了两次假阳性：rollup 的 module id 在 Windows 上一会儿是
> `D:/…` 一会儿是 `\0D:/…`（虚拟模块前缀）。规整之后才是真的在比路径。

**运行期**（`examples/host-react/src/main.tsx` 的 `ReactInstanceProbe`）：
宿主自己渲染一个组件，在里面调**包内创建的** zustand store 的 hook
（`useUiStore((s) => s.path.length)`）。两份 React 的话 dispatcher 对不上，
这里会直接抛 "Invalid hook call"。`e2e:http` 断言探针的 `data-hook-ok="1"`
且宿主与包报同一个 `React.version`。

peerDependency 的版本范围写在 [`packages/editor/README.md`](../packages/editor/README.md#装它)：
`react` / `react-dom` 是 `^18.3.1 || ^19.0.0`，`@xyflow/react` `^12.11.6`，
`three` `>=0.160.0 <1`，`zustand` `^5.0.2`，`@tauri-apps/api` `^2` 且**可选**
（浏览器宿主不装也能跑，那三处 `import()` 永远不执行）。

### 4. A2-5 的 UI 项

四项里前三项 A1 收尾时已经做了并有 e2e（`phase_a.mjs`：`plan_extended` 追加的节点、
`not_demanded` 的 `opacity < 0.6`、Error 类型的端口色）。本次补齐后两项：

- **「标为输出」右键项**：节点右键菜单按算子的输出端口逐个列出
  `标为输出：<port>`；已经标过的那个变成 `取消输出 <name>`。
  名字默认取端口名，重名自动加 `_2`；同一个端口标两次不会冒出两条。
  写进 doc 的是**展开后的路径 id**（`fullId(path, nodeId)`），所以在子图里标也说得清是哪一个。
  进撤销栈（`标为输出 out` / `取消输出 out`）。
- **Inspector 的图级输出列表**：钉在检查器最上面（它不属于任何一个选中节点），
  一行一个输出：名字、来自哪个 `node.port`、跑过之后补上实测值（复用 `formatOutputValue`），
  外加一个取消按钮。`doc.outputs` 为空时整段不渲染。

e2e 走的是真实交互路径：派发 `contextmenu` 事件 → 点菜单项 → 读 doc；
再从 DOM 上读 Inspector 那一行 → 点 ✕ → 确认 doc 和 DOM 都空了。

### 5. 文档

| 文件 | 内容 |
|---|---|
| `docs/http-transport.md` | **新增。** 端点清单（REST 路径 ↔ Transport 方法 ↔ C ABI v7 三列对照）、请求/响应 JSON、二进制点云布局表、WebSocket 帧与鉴权、桩服务器与真实服务的差别 |
| `docs/adr/0018-editor-as-package.md` | **新增。** 六条决定（拆包与源码发布、传输层注入、peerDependencies、`--lyflow-*`、快捷键挂根元素、对话框注入）+ 影响 + 没做 |
| `packages/editor/README.md` | **新增。** 安装、peer 版本范围、Props、Transport、对话框、主题变量表、快捷键、导出清单、目录；后半段是从 `app/README.md` 搬过来的内部约定与「踩过的坑」 |
| `app/README.md` | **重写。** 现在只讲壳：五个文件、Transport 的选择、对话框归壳、两条验收线 |
| `docs/architecture.md` | 职责表拆成「编辑器包」与「宿主壳」两行，加一段 Transport 的说明 |
| `scripts/e2e/README.md` | 新增 `pnpm e2e:http` 一节：跑法、前置条件、环境变量、它拉起哪三样东西 |

---

## 端点清单摘要

完整契约见 [docs/http-transport.md](http-transport.md)。基址下全部挂在 `/lyflow/`：

```
GET    /lyflow/manifest                                  → OperatorManifestBundle
GET    /lyflow/core-info                                 → CoreInfo
POST   /lyflow/validate        {doc, graphPath}          → GraphDiagnostic[]
POST   /lyflow/plan            {doc, graphPath, targets} → PlanNode[]（校验没过时是诊断数组）
POST   /lyflow/run             {doc, graphPath, targets, mode, previewMaxPoints, previewBudgetMs}
                                                         → {runId}
POST   /lyflow/cancel          {runId}                   → {}
GET    /lyflow/runs/:id/outputs                          → RunOutputs
GET    /lyflow/runs/:id/nodes/:node/outputs              → OutputInfo[]
GET    /lyflow/runs/:id/clouds/:node/:port?maxPoints=N   → application/octet-stream（'LYPC'）
GET    /lyflow/cache                                     → CacheStats
DELETE /lyflow/cache                                     → {}
GET    /lyflow/library                                   → LibraryStatus
POST   /lyflow/library/refresh                           → {status, manifest}
POST   /lyflow/library/save    {doc, subgraphId, meta}   → LibraryStatus
POST   /lyflow/import          {kind, text, baseDir}     → GraphDoc（失败 400 + 诊断）
GET    /lyflow/files/graph?path=                         → {doc, migrations}
PUT    /lyflow/files/graph?path=          {doc}          → {}
GET    /lyflow/files/backup?path=                        → {doc, migrations}
PUT    /lyflow/files/backup?path=         {doc}          → {}
DELETE /lyflow/files/backup?path=                        → {}
GET    /lyflow/files/backup/status?path=                 → BackupStatus
PUT    /lyflow/files/bytes?path=          <裸二进制>      → {}
GET    /lyflow/recent                                    → RecentEntry[]
POST   /lyflow/recent          {path}                    → RecentEntry[]
WS     /lyflow/events                                    → ExecutionEvent（+ 两种控制帧）
```

鉴权：HTTP 走 `Authorization: Bearer <token>`；WebSocket 走子协议
`lyflow.v1, lyflow-token.<token>`（**不进查询串** —— 查询串会进访问日志和 Referer）。

点云载荷与 `bridge/src/execution.rs` 的 `encode_cloud` 逐字节一致：
16 字节头（magic `0x4350594C` / pointCount / totalPoints / flags）+ 24 字节 bounds +
`f32[3n]` xyz + 可选 intensity + 可选 normals，全部小端。

---

## 偏离与决策

### 与 A2 决定表的偏离

1. **`Transport` 接口多一个 `importGraph`。** A2-2 的端点清单里有 `/import`，
   但原来的 `Transport` 没有对应方法（Tauri 侧 `import_graph` 这条 command 一直没人调）。
   补上它，三个实现都实现，否则「REST 与 Tauri command 一一对应」这句话在这一条上不成立。

2. **WebSocket 上多两种控制帧。** A2-2 说「消息体就是 ExecutionEvent」。
   执行事件确实是裸的、没有信封，但 `onManifestUpdated` 与 `onCoreReloadFailed`
   这两个订阅也只有这一条连接可走，于是加了 `kind: "manifest_updated"` /
   `"core_reload_failed"` 两种帧，靠 `kind` 与执行事件区分（执行事件的 `kind` 是
   `run_started` 等六个之一）。不支持热重载的后端一条都不发。

3. **`getCoreInfo` 对应的端点 `/lyflow/core-info` 不在 A2-2 的清单里。**
   清单列的是「与 Tauri command 一一对应」，而 `get_core_info` 是其中一条，补上。

4. **快捷键除了根元素还在 `ownerDocument` 上挂了一条兜底**，只处理
   `event.target === document.body` 的按键。理由与取舍见 ADR-0018 第五条；
   没有它 `pnpm e2e` 过不去（`Ctrl+E`、`?`、以及焦点被卸载之后的 F5）。
   宿主自己的输入框仍然完全不受影响 —— 那些事件的 target 不是 body。

5. **包按 TypeScript 源码发布，不出 `dist`。** A2-1 没说发布形态。
   两个消费方都是 Vite + TS，加一个构建步骤只会带来「dist 过期了」这类新故障。
   代价（宿主必须能编译 TS/TSX）写在 README 里。真要发 npm 时补一个 `tsc` 构建即可。

6. **`app/` 还多了两个文件：`title.ts` 与 `shell.css`。**
   A2-1 说壳留「入口、窗口、菜单、文件对话框、devbridge」。窗口标题原来在
   `App.tsx` 里（`document.title`），那是宿主页面的东西，跟着「窗口」一起归壳；
   `html/body/#root` 的高度与底色同理。

7. **`examples/host-react` 有自己的窗口桥**（`src/bridge.ts`），没有复用
   `app/src/devbridge.ts`。A2-1 把 devbridge 划给 `app/`，所以没有把它提进包里。
   宿主那一份只有 HTTP 子集用得到的字段（没有 `runMarks`、没有 `subgraphs`、
   没有 `cache.stale`）。**代价是两份桥会漂移** —— 记在这里。

8. **`TransportKind` 从两个值变成三个，live preview 的门禁跟着改。**
   `lib/preview.ts` 原来写的是 `if (transport.kind !== "tauri") return`，
   HTTP 下会静默失效。改成 `=== "static"`。

### 未做 / 已知缺口

- **桩服务器不是真实服务。** 每个请求起一次 `lyflow` CLI 进程，于是：
  缓存统计恒为 0（进程间没有共享结果仓）、取点云会**重跑一遍图**、
  `loadGraph` 的 `migrations` 恒为 `[]`、`saveAsLibrary` 返回 501、不发热重载帧。
  这些都写在 `docs/http-transport.md` 的最后一节，是桩的省略不是契约的一部分。
  阶段 B 的业务服务应该常驻 core（`core/include/lyflow/client.hpp`）。

- **`e2e:http` 是子集，不是 `e2e` 的等价物。** 29 条 vs 332 条。
  没覆盖的有：取消运行、子图、库算子、live preview、大图性能、备份恢复、
  最近文件、迁移、拖拽手感、右键菜单的其余项。这些的正确性由 Tauri 那条线保证 ——
  它们不经过传输层的差异面（都是同一份包代码）。真正只在 HTTP 下会坏的是
  「请求怎么发、事件怎么收、二进制怎么解」，那三样覆盖到了。

- **`e2e:http` 没进 `pnpm check`。** 它要拉浏览器，与 `pnpm e2e` 一样是单独一条命令。

- **一个页面里只能有一个编辑器实例。** stores 与 transport 都是模块级单例。
  两个 `<LyFlowEditor>` 会共用一份文档，后挂载的那个的 transport 会赢。
  README 与 ADR 里都写了。要做得把 store 建进 React context，是一次不小的改造。

- **`pnpm e2e:packaged` 没跑。** 打包链路这次没碰，标未验证（A1 也是这么记的）。

- **无头模式没验。** `LYFLOW_E2E_HEADLESS=1` 这条路留了，但 3D 那条断言要
  SwiftShader 才画得出来，没实测过。默认是有头的。

- **`body.is-resizing` / `body.param-dragging` 两个全局类名还留在包里。**
  它们只在拖动过程中存在（改鼠标指针、禁选中），没有改成 `--lyflow-` 前缀的类名。
  宿主页面里真撞上了才值得改。

---

## 复现命令

```powershell
cd D:\project\LyFlow
pnpm install

# 1. 两条门禁
pnpm check
$env:LYFLOW_PACKS="gap"; pnpm check:gap

# 2. Tauri 壳的 e2e（332 条）
$env:LYFLOW_PACKS="gap"; node scripts/e2e/run.mjs

# 3. HttpTransport 的 e2e（29 条）
#    前置：bridge\target\debug\lyflow.exe 存在（第 1 步会建好）
node scripts/e2e/http.mjs

# 4. 手动看一眼浏览器宿主
node packages/editor/test-server/server.mjs --root <一个工作区目录> `
     --cli bridge\target\debug\lyflow.exe --port 8787
pnpm host:dev            # http://127.0.0.1:5174
```

第 2、3 步会各自开一个窗口，跑完自己收掉。**跑的时候别改前端源码** ——
Vite HMR 会把页面重载，断言会以很费解的方式失败。

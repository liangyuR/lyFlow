# 库算子「展开为内联子图」—— 实施说明

> 状态：**已实施**（2026-09-29）。C 档第 3 项。取定义的方式按方案 1（core 出定义，C ABI v13）。

## 现状

右键库算子节点（`lib.<id>`）有一项「展开为内联子图」，点了只弹一条警告。库算子的定义只在 core 里
（`exec::Library`，扫描 `*.lyflow-op.json` 时解析成 `SubgraphDef`），前端与 bridge 都拿不到。

## 做法

| 层 | 改动 |
|---|---|
| core | `Library` 扫描时把每个文件的原始 JSON 一并留下；`definitionJson(opId)` 返回**去掉 `id` 的那份对象**（其余键 —— name / doc / category / version / keywords / nodes / edges / inputs / outputs / params —— 与图文档里 `subgraphs.<id>` 的形状一致，category 是文件里写的，不带 `Library/` 前缀） |
| C ABI v13 | `char* lyflow_library_definition(const char* op_id)`：找到返回对象 JSON（`{` 开头），找不到返回空串。只加函数不改结构，但宿主按版本号严格比对，照例升一版 |
| bridge / Rust client | `library_definition(opId) -> Option<Value>`；命令 `get_library_definition` |
| HTTP | `GET /lyflow/library/definition?op=lib.<id>`；桩服务器没有库目录，回 501（与「保存到库」同） |
| 前端 | `Transport.getLibraryDefinition(opId)`；纯函数 `inlineLibraryNode(doc, path, nodeId, def)`（`lib/subgraph.ts`）：定义以新 id（`newLocalId("sg", …)`）写进 `doc.subgraphs`，节点的 `op` 从 `lib.<id>` 换成 `sub:<新 id>`；节点 id、参数、连线原样不动（子图的提升参数与端口就是库算子的参数与端口）。graph store 的 `inlineLibrary` 包成一条撤销「展开库算子」 |
| 界面 | 右键「展开为内联子图」→ 取定义 → 展开 → 选中它、toast「已展开为子图，可双击进入编辑；库文件不受影响」。取不到（库文件已删、HTTP 桩）时 toast 说原因 |

## 边界

- **只展开一层**：定义里的节点若还是 `lib.*`，保持库算子（可以再展开）。库文件本来就不许嵌 `sub:`（`save_as_library` 拦着），不会展开出多层定义。
- **展开后与库文件脱钩**：之后改库文件不影响这张图，这正是「内联」的意思。
- **行为不变**：展开前后 compile 出来的是同一张平图（库算子与 `sub:` 走同一条展开路径，ADR-0010）。

## 测试

- core doctest：`definitionJson` 返回去掉 `id` 的对象、找不到返回空；放进已有的库算子用例。
- editor 单测：`inlineLibraryNode` 表驱动（op 换成 `sub:`、定义写入且 id 不撞、参数与连线不动、不是库节点 / 没有定义时不改）。
- e2e `m4.mjs` 的 §1 库算子分组里续一段：右键展开 → 节点变成 `sub:`、再跑一次结果与展开前相同、一次撤销回到 `lib.`。

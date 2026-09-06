# ADR-0017：图有命名输出，源数据可以注入，文本可以导入成图

日期：2026-09-06　状态：已采纳

## 背景

`xyz-gap-inspector` 要在自己的进程里加载 core DLL、每个测点跑一张图
（[gap-inspector-integration-design.md](../gap-inspector-integration-design.md) §1）。
把 LyFlow 从「编辑器的后端」变成「可嵌入的执行引擎」，缺三样东西。

**一、结果怎么取。** 现有的口子是 `lyflow_output_info(runId, nodeId)` —— 按**节点 id**取。
业务侧于是要硬编码 `n_gap`、`n_flush` 这些 id，而 id 是图作者随手起的名字：
用户在编辑器里重命名一个节点，产线就停了。

**二、输入怎么进。** 相机采到两片云已经在内存里。现有的路是先写临时 PCD、
再让 `gap.load_profile_pair` 读回来。一次测量多两次磁盘往返，还多一个「临时文件谁来删」
的问题。

**三、194 个测点的老配置怎么变成图。** 迁移脚本得能把 `StandardGap.yml` 转成
`.lyflow.json`。Python 生成器（`packs/gap/tools/lyflow_graph_from_config.py`）已经能做，
但业务的 `PrepareDatabase` 是 C++，不该为此绑一个 Python 运行时。

## 决定

### 一、GraphDoc 顶层 `outputs`

```json
"outputs": {
  "gap":    { "node": "n_gap",    "port": "value" },
  "flush":  { "node": "n_flush",  "port": "value" },
  "bundle": { "node": "n_bundle", "port": "bundle" }
}
```

宿主只认名字。`lyflow_run_outputs(run_id)` 返回

```json
{ "gap": { "node": "n_gap", "port": "value", "type": "Measurement",
           "elementCount": 1, "byteSize": 96, "value": { ... } } }
```

点云只给元信息，二进制仍走 `lyflow_output_cloud`（D4 不变）。
该端口没有结果时（节点没跑到、惰性闭包没被 demand）多一个 `"missing": true`，
**不是**把这一项整个省掉 —— 宿主拿到的键集合应当只由图决定，不由运行结果决定。

校验在 `buildPlan` 里：节点存在、端口存在于那个算子上。名字唯一由 JSON 对象本身保证。
子图内部的端口写**展开后**的路径 id（`outer/inner`），与 `run_started.plan` 里的 id 同一套。
`run_started` 事件也带上 `outputs` 声明，前端不必再自己解析图。

存放在结果仓（`ResultStore::setNamedOutputs`）而不是 `Run` 对象上：取输出的时机是
`lyflow_run_join` 之后、`lyflow_run_free` 之前，而那两个之间只有 runId 是共通的。

### 二、`lyflow_run_options.inputs`

```c
typedef struct {
  const char* node_id;
  const char* port;
  int32_t kind;            /* 目前只有 LYFLOW_INPUT_POINT_CLOUD */
  uint32_t count;
  const float* xyz;        /* 3 * count */
  const float* intensity;  /* count，或 NULL */
} lyflow_run_input;
```

被注入的节点在编译期标 `provided`：**compute 整个不调用**，输出直接从注入数据装配，
事件里是 `node_state: done` + `stats.provided = true`。

一个节点只要被注入一次，它声明的**每个**输出端口都得给一项。少给的表现是
「算子没有写输出端口 X」—— 这条错误已经存在，且指向明确，所以不额外发明一条。

注入数据的 xxh3 摘要进 cacheKey。这条是必须的：不进键的话，换一片云会命中上一片云的结果，
而现象是「产线上两台车量出一模一样的值」。

缓冲由调用方持有到 `lyflow_run_start` 返回，core 在里面拷一份 —— 跨 ABI 边界共享
生命周期是最容易出错的一类约定，宁可多拷一次。

### 三、导入器

```cpp
using ImportFn = Status (*)(const std::string& text, const std::filesystem::path& baseDir,
                            std::string& graphJson);
Registry::addImporter(ImporterDesc{kind, label, doc, pack, fn});
```

C ABI 是 `lyflow_import(kind, text, base_dir)`：成功返回 GraphDoc 对象（`{` 开头），
失败返回诊断数组（`[` 开头）。这与 `lyflow_plan` 的两种返回值是同一套区分办法
（[ADR-0007](0007-cache-authority.md)），调用方已经会写那段判断。

注册了的导入器进 **manifest** 的 `importers` 段，而不是新开一个 C 函数。
理由是 ABI 在 v7 之后要冻结一整个阶段 B，能不加就不加；而 manifest 本来就是
「前端能看见什么」的唯一来源。

CLI：`lyflow import <file> --kind <kind> [-o out.lyflow.json] [--base-dir dir]`。
导入器产出的图会先过一遍 `GraphDoc::validate_structure` 再落盘 —— 坏图不该写进数据库。

## C ABI v7

一次加齐，之后阶段 B 期间冻结：

| 新增 | 形态 |
|---|---|
| `lyflow_run_outputs` | `char* (const char* run_id)` |
| `lyflow_import` | `char* (const char* kind, const char* text, const char* base_dir)` |
| `lyflow_run_options.inputs` | `const lyflow_run_input*` |
| `lyflow_run_options.input_count` | `size_t` |

`lyflow_run_options` 尾部追加两个字段是**破坏性**的（结构体按值传，布局变了），
所以版本号从 6 跳到 7，头文件里有 `LYFLOW_ABI_VERSION`，
`client.hpp` 与 Rust 的 `core_ffi::ABI_VERSION` 各持一份常量。
符号缺失会在加载时报「找不到符号 X（ABI 不匹配）」而不是在调用时崩。

## 后果

- ✅ 业务侧的耦合从「节点 id」降到「三个名字」。编辑器里怎么改图都不影响产线。
- ✅ 相机 → 内存 → 图，中间没有磁盘。这也是以后相机直连 LyFlow 的口子。
- ✅ 数据库迁移是纯 C++，Python 生成器降级成离线工具。
- ❌ ABI 破坏了一次。装了旧 core DLL 的宿主会在加载期报错而不是静默跑错，
  这是刻意的，但升级时两边必须一起换。
- ❌ 注入的语义是「整个节点」而不是「一个端口」。想只替换双输出算子的一个输出，
  当前做不到 —— 真需要时应当拆算子，而不是让执行器去猜另一个输出从哪来。
- ❌ `outputs` 用展开后的路径 id 指子图内部的端口。子图重命名会打断它，
  而 GraphDoc 里没有别的东西能稳定地指向一个内部端口。

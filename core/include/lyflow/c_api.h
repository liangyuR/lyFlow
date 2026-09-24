#ifndef LYFLOW_C_API_H
#define LYFLOW_C_API_H
// C ABI v11。Rust 桥接层与嵌入宿主（include/lyflow/client.hpp）只看见这个头文件。
// 三条约定（char* 归属、异常不跨 ABI、只导出 C 函数）见 core/README.md「C ABI 约定」。
#define LYFLOW_ABI_VERSION 11
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(LYFLOW_BUILDING_DLL)
#    define LYFLOW_API __declspec(dllexport)
#  else
#    define LYFLOW_API __declspec(dllimport)
#  endif
#else
#  define LYFLOW_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- 描述与自检

// core 的版本号，形如 "0.1.0"。静态存储，不要释放。
LYFLOW_API const char* lyflow_version(void);

// 全量算子描述，符合 schema/operator-manifest.schema.json。
LYFLOW_API char* lyflow_manifest_json(void);

// 注册表自检结果，每行一条问题；无问题返回空字符串。
// 桥接层应在启动时调用一次，有内容就当作 fatal —— 契约破了继续跑没有意义。
LYFLOW_API char* lyflow_manifest_problems(void);

LYFLOW_API void lyflow_string_free(char* s);

// ------------------------------------------------------------------ 库算子

// 设置库算子目录（ADR-0010）：每个 `*.lyflow-op.json` 注册成算子 `lib.<id>`。
// 返回问题列表（每行一条，空串 = 干净）。调用时必须没有活跃的 run。
LYFLOW_API char* lyflow_set_library_dirs(const char* const* dirs, size_t n);

// 当前注册了几个库算子。
LYFLOW_API size_t lyflow_library_count(void);

// ------------------------------------------------------------------ 只校验

// 同步校验一张图，不执行。返回诊断 JSON 数组（可能是空数组 "[]"）。
// 每一项带 kind："diagnostic" 是普通诊断，"migration" 另带 op/opVersion/params/notes。
LYFLOW_API char* lyflow_validate(const char* graph_json, const char* base_dir);

// 同 lyflow_validate，另带顶层图参数的取值（与 lyflow_run_options.params_json 同义：
// JSON 对象 { 名字: 值 }，NULL/空串 = 全用 default）。校验的是这组取值下的图 ——
// 编辑器拿「default + 当前配方」校验（param-recipe K5），宿主拿它在切换配方前先问一句。
// v11 里追加，ABI 号不变。
LYFLOW_API char* lyflow_validate_params(const char* graph_json, const char* base_dir,
                                        const char* params_json);

// -------------------------------------------------------------- 计划与缓存

// 编译一次，报告每节点 { nodeId, cacheKey, cached, level, upstreamMissing, bypass }。
// 校验失败时返回的是 lyflow_validate 那种诊断数组，靠 kind 字段区分（ADR-0007）。
LYFLOW_API char* lyflow_plan(const char* graph_json, const char* base_dir,
                             const char* const* targets, size_t n);

// 同 lyflow_plan，另带顶层图参数的取值（同 lyflow_validate_params）。cacheKey 与 cached
// 反映的是这组取值：切配方之后「哪些节点会重算」靠它（v11 里追加）。
LYFLOW_API char* lyflow_plan_params(const char* graph_json, const char* base_dir,
                                    const char* const* targets, size_t n,
                                    const char* params_json);

// 进程级地丢掉全部缓存结果，别的 run 的结果也一并丢，所以调用方应当先取消。
// 只想让某一次运行不吃缓存的，用 lyflow_run_options.no_reuse。
LYFLOW_API void lyflow_cache_clear(void);

// { entries, bytes, budgetBytes, hits, misses, evictions } 的 JSON 对象。
LYFLOW_API char* lyflow_cache_stats(void);

// -------------------------------------------------------------------- 执行

// 事件 JSON 符合 schema/execution-event.schema.json。
// 回调在 core 的工作线程上发生，join 返回之后不会再被调用。
typedef void (*lyflow_event_cb)(const char* event_json, void* user);

typedef struct lyflow_run lyflow_run;

/* 运行时注入一个源节点的输出（v7）。被注入的节点整个 compute 都不会被调用，
   所以它声明的**每个**输出端口都要给一项，否则执行器报 output_not_written。
   port 只是该算子的**输入**端口（不是输出端口）时是输入注入（m8-plan L18，加在 v10 里）：
   compute 照常调用，这个输入端口的值就是注入的数据；端口上不能同时有连线（bad_input），
   同一个节点也不能既注入输出又注入输入。例如 gap.read_scan 的 primary / secondary。
   缓冲由调用方持有，必须活到 lyflow_run_start 返回为止（core 内部会拷一份）。 */
typedef struct {
  const char* node_id;
  const char* port;
  int32_t kind;                    /* 目前只支持 LYFLOW_INPUT_POINT_CLOUD */
  uint32_t count;                  /* 点数 */
  const float* xyz;                /* 3 * count */
  const float* intensity;          /* count，或 NULL */
  const float* normals;            /* 3 * count，或 NULL */
  const uint8_t* rgb;              /* 3 * count，或 NULL */
} lyflow_run_input;

#define LYFLOW_INPUT_POINT_CLOUD 0

typedef struct {
  const char* run_id;              /* 由调用方分配（Rust 用 ULID），事件里原样回传 */
  const char* base_dir;            /* 相对路径参数的基准目录，可为 NULL */
  const char* const* targets;      /* Run to node 的目标节点，NULL/0 表示跑全图 */
  size_t target_count;
  int32_t max_parallel;            /* 同时跑几个节点。0 = min(4, 核数)，1 = 顺序执行 */
  uint64_t cache_budget_bytes;     /* 结果仓字节预算。0 = min(8 GB, 物理内存 40%) */
  int32_t mode;                    /* 0 = full，1 = preview（源算子输出先抽稀，ADR-0011） */
  uint32_t preview_max_points;     /* preview 的点数上限。0 = 200000 */
  uint32_t preview_budget_ms;      /* 超过它发一条 warn 日志。0 = 300 */
  int32_t no_reuse;                /* 非 0 = 本次运行不复用结果仓里的旧结果 */
  const lyflow_run_input* inputs;  /* v7：运行时注入的源数据，NULL/0 表示没有 */
  size_t input_count;
  /* v10：顶层图参数的取值，JSON 对象 { 名字: 值 }（GraphDoc 顶层 params）。
     NULL/空串 = 全用图里的 default。图没声明的名字在校验阶段报 unknown_param。 */
  const char* params_json;
  /* v11：只运行这些节点（docs/node-run-plan.md R1–R2），NULL/0 表示普通运行。id 语义与
     targets 相同（子图节点按路径前缀展开为全部内部节点）；给了它就忽略 targets、改用同一组 id。
     不在里面的上游只许命中缓存，任何一个缺当前 cacheKey 的结果就在开跑前整次失败：
     run_finished 为 error，error.code 与 diagnostics[].code 是 upstream_not_ready，
     每个缺结果的上游一条（nodeId 指它），不执行任何算子。在里面的节点自己照常查缓存（要真跑
     一遍另给 force）。与 mode = preview 同时给是参数错误（bad_input）。 */
  const char* const* isolate;
  size_t isolate_count;
  /* v11：强制重算这些节点（修订一 V1），NULL/0 表示没有。id 语义同 targets；跳过缓存、真跑、
     结果覆盖同 cacheKey 的旧结果。可与 targets、isolate、preview 组合。
     带 targets（或 isolate）的运行，计划外、结果仓里有当前 cacheKey 结果的节点挂进这次运行，
     run_finished.attached 列出它们，输出按这次的 run_id 照样取得到（R7 / V2）。 */
  const char* const* force;
  size_t force_count;
} lyflow_run_options;

#define LYFLOW_RUN_MODE_FULL 0
#define LYFLOW_RUN_MODE_PREVIEW 1

// 立即返回句柄，执行在后台线程。opts/cb 允许为 NULL（cb 为 NULL 时事件丢弃）。
// 返回 NULL 表示连线程都没起来（内存不足），调用方应当当作运行失败。
LYFLOW_API lyflow_run* lyflow_run_start(const char* graph_json,
                                        const lyflow_run_options* opts,
                                        lyflow_event_cb cb, void* user);

// 协作式取消。可重复调用，也可在 join 之后调用（无操作）。
LYFLOW_API void lyflow_run_cancel(lyflow_run* run);

// 等到工作线程退出。返回后保证 cb 不会再被触发 —— Rust 侧据此安全释放 user。
LYFLOW_API void lyflow_run_join(lyflow_run* run);

// 必须在 join 之后调用。同时释放该 run 在结果仓里的全部结果。
LYFLOW_API void lyflow_run_free(lyflow_run* run);

// ------------------------------------------------------------------ 结果仓

// D4：结果留在 C++，点云走二进制，绝不 JSON。
// 一百万个点的 JSON 数组是 30MB 文本 + 前端一次全量解析，这条路走不通。
//
// 下面凡是带 port 参数的取数函数（output_cloud / tensor / indices / save）都认
// `<port>.<field>`：取 Bundle 端口里的一个字段，例如 "scan.merged"（m8-plan L3，
// 加在 v10 里，签名不变、ABI 号不变）。字段本身又是什么类型，就按那个类型的规矩取。
typedef struct {
  uint32_t point_count;   /* 抽样后的点数 */
  uint32_t total_points;  /* 抽样前的点数 */
  uint32_t flags;         /* bit0: 带 intensity；bit1: 带 normals */
  uint32_t reserved;
  float bounds[6];        /* minx,miny,minz,maxx,maxy,maxz；空云时全 0 */
  const float* xyz;       /* 3 * point_count */
  const float* intensity; /* point_count，或 NULL */
  const float* normals;   /* 3 * point_count，或 NULL */
  void* handle;           /* 内部持有，勿动 */
} lyflow_cloud_view;

#define LYFLOW_CLOUD_HAS_INTENSITY 1u
#define LYFLOW_CLOUD_HAS_NORMALS 2u

// 取某个节点某个输出端口的点云，等步长抽样到 max_points 以内。
// 返回 0 = 成功；非 0 = 没有这个结果 / 该输出不是点云。
LYFLOW_API int lyflow_output_cloud(const char* run_id, const char* node_id, const char* port,
                                   uint32_t max_points, lyflow_cloud_view* out);

LYFLOW_API void lyflow_cloud_view_free(lyflow_cloud_view* view);

// v8：张量按切片取（ADR-0019）。与点云不同，这里**零拷贝** ——
// shape/data 直接指进结果仓里那一份，handle 一释放就失效。
typedef struct {
  uint32_t rank;
  uint32_t count;         /* 本次返回的元素数 */
  uint64_t offset;        /* 本次切片的起始元素下标 */
  uint64_t total;         /* 张量总元素数 */
  const int64_t* shape;   /* rank 个，永远是完整形状，不随 offset/count 变 */
  const float* data;      /* count 个 */
  void* handle;           /* 内部持有，勿动 */
} lyflow_tensor_view;

// offset 越界返回 count=0；count=0 表示「从 offset 取到末尾」。
// 返回 0 = 成功；1 = 没有这个结果 / 该输出不是张量；2 = out 为空；3 = 异常。
LYFLOW_API int lyflow_output_tensor(const char* run_id, const char* node_id, const char* port,
                                    uint64_t offset, uint32_t count, lyflow_tensor_view* out);

LYFLOW_API void lyflow_tensor_view_free(lyflow_tensor_view* view);

// v8：点下标集合，同样零拷贝按切片取。
typedef struct {
  uint32_t count;            /* 本次返回的下标个数 */
  uint32_t total;            /* 下标总数 */
  uint64_t source_cloud_id;  /* 指向哪片云；前端只显示，不校验 */
  const int32_t* values;     /* count 个 */
  void* handle;              /* 内部持有，勿动 */
} lyflow_indices_view;

// 越界与返回码的约定与 lyflow_output_tensor 完全一致，1 表示该输出不是下标集合。
LYFLOW_API int lyflow_output_indices(const char* run_id, const char* node_id, const char* port,
                                     uint64_t offset, uint32_t count, lyflow_indices_view* out);

LYFLOW_API void lyflow_indices_view_free(lyflow_indices_view* view);

// 某节点全部输出的 { port, type, elementCount, byteSize, value? } JSON 数组。
// Bundle 端口（type 是 "Bundle<kind>"）那一项之后紧跟它的每个字段，port 写成 <port>.<field>。
LYFLOW_API char* lyflow_output_info(const char* run_id, const char* node_id);

// v7：图级命名输出（GraphDoc 顶层 outputs）。返回
// { 名字: { node, port, type, elementCount, byteSize, value?, missing? } }。
// 图输出的 port 可以是 <port>.<field>（指向 Bundle 的一个字段），这里照样给那个字段的值。
// 点云只给元信息，二进制仍走 lyflow_output_cloud。图没声明 outputs 时返回 "{}"。
LYFLOW_API char* lyflow_run_outputs(const char* run_id);

// v9：一次运行的结构化收尾（ADR-0022）。返回
// { runId, status, durationMs, nodes, outputs, decisions, contractViolations }。
// status 三态 ok | degraded | failed；outputs 每一维三态 value | inactive | failed。
// **run 结束之前返回 NULL** —— summary 由执行器在发 run_finished 之前登记，
// 所以调用方应当先 lyflow_run_join。lyflow_run_free 之后同样返回 NULL。
LYFLOW_API char* lyflow_run_summary(const char* run_id);

// v9（本轮加在 v9 里，ABI 号不变）：每节点每参数的生效值与来源（m6-plan §2）。返回
// { nodes: [ { node, op, params: [ { param, value, source, label?, unit?, min?, max? } ] } ] }，
// source 是 "default" | "explicit" | "bound"（子图提升参数灌进来的，ADR-0010）|
// "graph"（v10：顶层图参数灌进来的，该项另带 graphParam: "<名字>"）。
// 稀疏存储是对的，但「现在到底跑的是什么值」必须由合并默认值、跑完迁移的那一层说，
// 在桥接层重算一遍迟早与执行器漂开。
// 校验有错时返回**诊断数组**（'[' 开头），与 lyflow_plan 同一套区分办法。base_dir 可为 NULL。
LYFLOW_API char* lyflow_effective_params(const char* graph_json, const char* base_dir);

// v7：走注册好的导入器把一段文本变成图（比如 gap 包的 "StandardGap.yml"）。
// 成功返回 GraphDoc 对象（'{' 开头），失败返回诊断数组（'[' 开头），
// 与 lyflow_plan 的两种返回值同一套区分办法（ADR-0007）。
// 可用的 kind 见 manifest 的 importers 段。base_dir 可为 NULL。
LYFLOW_API char* lyflow_import(const char* kind, const char* text, const char* base_dir);

// 把某个输出整份写到磁盘（PCD/PLY 按扩展名）。CLI 的 `lyflow dump` 用它 ——
// 写盘格式的知识留在 core。返回空串 = 成功，否则是一句人话的失败原因。
LYFLOW_API char* lyflow_output_save(const char* run_id, const char* node_id, const char* port,
                                    const char* path, const char* format);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LYFLOW_C_API_H

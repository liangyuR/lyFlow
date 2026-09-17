#ifndef LYFLOW_C_API_H
#define LYFLOW_C_API_H
// C ABI v9。Rust 桥接层与嵌入宿主（include/lyflow/client.hpp）只看见这个头文件。
// 三条约定（char* 归属、异常不跨 ABI、只导出 C 函数）见 core/README.md「C ABI 约定」。
#define LYFLOW_ABI_VERSION 9
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

// -------------------------------------------------------------- 计划与缓存

// 编译一次，报告每节点 { nodeId, cacheKey, cached, level, upstreamMissing, bypass }。
// 校验失败时返回的是 lyflow_validate 那种诊断数组，靠 kind 字段区分（ADR-0007）。
LYFLOW_API char* lyflow_plan(const char* graph_json, const char* base_dir,
                             const char* const* targets, size_t n);

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

// 某节点全部输出的 { port, type, elementCount, byteSize } JSON 数组。
LYFLOW_API char* lyflow_output_info(const char* run_id, const char* node_id);

// v7：图级命名输出（GraphDoc 顶层 outputs）。返回
// { 名字: { node, port, type, elementCount, byteSize, value?, missing? } }。
// 点云只给元信息，二进制仍走 lyflow_output_cloud。图没声明 outputs 时返回 "{}"。
LYFLOW_API char* lyflow_run_outputs(const char* run_id);

// v9：一次运行的结构化收尾（ADR-0022）。返回
// { runId, status, durationMs, nodes, outputs, decisions, contractViolations }。
// status 三态 ok | degraded | failed；outputs 每一维三态 value | inactive | failed。
// **run 结束之前返回 NULL** —— summary 由执行器在发 run_finished 之前登记，
// 所以调用方应当先 lyflow_run_join。lyflow_run_free 之后同样返回 NULL。
LYFLOW_API char* lyflow_run_summary(const char* run_id);

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

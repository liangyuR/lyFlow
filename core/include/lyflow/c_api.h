#ifndef LYFLOW_C_API_H
#define LYFLOW_C_API_H
//
// C ABI v2。Rust 桥接层只看见这个头文件。
//
// 约定：
//   * 所有返回 char* 的函数都返回 UTF-8、NUL 结尾的堆内存，调用方必须用
//     lyflow_string_free 释放。返回 NULL 表示分配失败。
//   * **异常绝不跨 ABI。** c_api.cpp 的每个入口都是 try/catch(...)，
//     内部异常转成返回码或事件里的 internal 错误。跨 DLL 边界抛异常在
//     MSVC 上是可以「工作」的，但一旦 Rust 侧栈帧介入就是未定义行为。
//   * D1：这个 DLL 只导出下面这些 C 函数。Registry / Executor 这些 C++ 类
//     不导出 —— 导出 C++ 类等于把 ABI 焊死在编译器版本上，而 M3 要热重载。
//
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

// ------------------------------------------------------------------ 只校验

// 同步校验一张图，不执行。返回诊断 JSON 数组（可能是空数组 "[]"）。
// 每一项：{ nodeId, severity, phase, code, message, paramPath?, portName? }
LYFLOW_API char* lyflow_validate(const char* graph_json, const char* base_dir);

// -------------------------------------------------------------------- 执行

// 事件 JSON 符合 schema/execution-event.schema.json。
// 回调在 core 的工作线程上发生，join 返回之后不会再被调用。
typedef void (*lyflow_event_cb)(const char* event_json, void* user);

typedef struct lyflow_run lyflow_run;

typedef struct {
  const char* run_id;              /* 由调用方分配（Rust 用 ULID），事件里原样回传 */
  const char* base_dir;            /* 相对路径参数的基准目录，可为 NULL */
  const char* const* targets;      /* Run to node 的目标节点，NULL/0 表示跑全图 */
  size_t target_count;
} lyflow_run_options;

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
  uint32_t flags;         /* bit0: 带 intensity */
  uint32_t reserved;
  float bounds[6];        /* minx,miny,minz,maxx,maxy,maxz；空云时全 0 */
  const float* xyz;       /* 3 * point_count */
  const float* intensity; /* point_count，或 NULL */
  void* handle;           /* 内部持有，勿动 */
} lyflow_cloud_view;

#define LYFLOW_CLOUD_HAS_INTENSITY 1u

// 取某个节点某个输出端口的点云，等步长抽样到 max_points 以内。
// 返回 0 = 成功；非 0 = 没有这个结果 / 该输出不是点云。
LYFLOW_API int lyflow_output_cloud(const char* run_id, const char* node_id, const char* port,
                                   uint32_t max_points, lyflow_cloud_view* out);

LYFLOW_API void lyflow_cloud_view_free(lyflow_cloud_view* view);

// 某节点全部输出的 { port, type, elementCount, byteSize } JSON 数组。
LYFLOW_API char* lyflow_output_info(const char* run_id, const char* node_id);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LYFLOW_C_API_H

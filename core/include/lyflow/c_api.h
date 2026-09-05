#ifndef LYFLOW_C_API_H
#define LYFLOW_C_API_H
//
// C ABI 边界。Rust 桥接层只看见这个头文件。
//
// 约定：所有返回 char* 的函数都返回 UTF-8、NUL 结尾的堆内存，
// 调用方必须用 lyflow_string_free 释放。返回 NULL 表示分配失败。
//
#ifdef __cplusplus
extern "C" {
#endif

// core 的版本号，形如 "0.1.0"。静态存储，不要释放。
const char* lyflow_version(void);

// 全量算子描述，符合 schema/operator-manifest.schema.json。
char* lyflow_manifest_json(void);

// 注册表自检结果，每行一条问题；无问题返回空字符串。
// 桥接层应在启动时调用一次，有内容就当作 fatal —— 契约破了继续跑没有意义。
char* lyflow_manifest_problems(void);

void lyflow_string_free(char* s);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LYFLOW_C_API_H

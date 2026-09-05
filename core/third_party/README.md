# third_party —— vendored 单头依赖

D7：这三个都不走 vcpkg。理由是 core 自身的构建必须保持秒级 —— 让 vcpkg 参与
进来，「加一个算子」的反馈循环就会被拖垮。PCL 是不得不付的代价，其余不是。

| 目录 | 版本 | 许可 | 用途 |
|---|---|---|---|
| `nlohmann/json.hpp` | v3.11.3 | MIT | GraphDoc 的 JSON 解析（写出仍然用自己的 `JsonWriter`，键顺序要可控） |
| `doctest/doctest.h` | v2.4.11 | MIT | C++ 测试框架 |
| `xxhash/xxhash.h` | v0.8.2 | BSD-2-Clause | cacheKey 的 XXH3-128。`XXH_INLINE_ALL` 方式引入，无需 .c |

升级方式：直接替换文件并更新上表。不要手改内容。

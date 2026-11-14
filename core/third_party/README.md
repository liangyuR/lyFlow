# third_party —— vendored 单头依赖

D7：这三个都不走 vcpkg。理由是 core 自身的构建必须保持秒级 —— 让 vcpkg 参与
进来，「加一个算子」的反馈循环就会被拖垮。PCL 是不得不付的代价，其余不是。

| 目录 | 版本 | 许可 | 用途 |
|---|---|---|---|
| `nlohmann/json.hpp` | v3.12.0 | MIT | GraphDoc 的 JSON 解析（写出仍然用自己的 `JsonWriter`，键顺序要可控） |
| `doctest/doctest.h` | v2.5.3 | MIT | C++ 测试框架 |
| `xxhash/xxhash.h` | v0.8.4 | BSD-2-Clause | cacheKey 的 XXH3-128。`XXH_INLINE_ALL` 方式引入，无需 .c |

升级方式：直接替换文件并更新上表。不要手改内容。

2026-11-15 按官方非预发布标签更新，保留上游原始文件，完整许可证位于各目录。固定源码：

| 依赖 | 官方修订 | 文件 SHA256 |
|---|---|---|
| [json v3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0) | 55f93686c01528224f448c19128836e7df245f72 | aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63 |
| [doctest v2.5.3](https://github.com/doctest/doctest/releases/tag/v2.5.3) | 2d0a9359a60c51affe2a9bebb1be1dca47868151 | cfd518a3ef90f67e1f3ba514df23fb3627437de1a2feeba78cf5062a40021421 |
| [xxHash v0.8.4](https://github.com/Cyan4973/xxHash/releases/tag/v0.8.4) | c87183a77d67f7d37e3d2d1b7eaac5e7c695e4f0 | 3dc8d161e867a62d3417f7885b55fdaded8b7b497e2f165d05c94f4fe24f2ca4 |

这里的校验仅用于第三方源码来源，不作为LyFlow产品或运行库身份。json的C++类型变化要求完整重编译；公共C ABI不暴露该类型。xxHash继续使用稳定XXH3-128，不改变缓存键协议。

# 主要原生依赖稳定版核对

核对日期：2026-11-15。以官方 tag、release 和固定 vcpkg baseline 的真实端口内容为依据；开发分支、CMake 内版本号和 GitHub 的 Latest 标签不单独作为正式稳定发布的证明。

## 盘点与本次范围

| 依赖 | 升级前固定来源 | 官方稳定版本/标签 | 本次处理 |
|---|---|---|---|
| Boost.Filesystem | 1.92.0 | 1.92.0 | 保持；1.93.0.beta1 是 beta |
| Eigen | 5.0.1 | 5.0.1 | 保持 |
| yaml-cpp | 0.9.0#1 | 0.9.0 | 保持 vcpkg 修订 |
| ONNX Runtime CPU | 1.19.2 | 1.32.0 | 升级官方 Windows x64 CPU 资产 |
| oneTBB | v2022.0.0 | v2023.2.0 | 升级固定源码，保留动态 tbb12 |
| KISS-Matcher | 8b58b118… | v1.0.2，e3440b63… | 改为正式 tag，同一 C++ 算法源码 |
| small_gicp | fa0cfc98… | tag v1.0.1，57c1106d… | 保留现有包含必要修复的固定提交 |
| ROBIN | 5623f02f… | v.1.2.7，对应同一提交 | 保持无 PMC 安全补丁 |
| xenium | 1c449ae9… | v0.0.2，2020 年 | 保留已验证固定提交，不倒退 |
| nlohmann/json | 3.11.3 | 3.12.0 | 更新core原始单头文件，完整重编 |
| doctest | 2.4.11 | 2.5.3 | 更新原始测试框架头，回归门禁另验 |
| xxHash | 0.8.2 | 0.8.4 | 更新原始单头，继续XXH3-128协议 |

OpenCV另从官方5.1.0固定源码构建，PCL保持最新稳定1.15.1#1；兼容层和算法验证由同一阶段的独立依赖工作项记录，不把它们的版本变化误算作匹配后端或推理后端的质量改善。发包与CI接入见下文。

官方依据：[Boost 1.92](https://github.com/boostorg/boost/releases/tag/boost-1.92.0)、[Eigen](https://libeigen.gitlab.io/)、[yaml-cpp 0.9](https://github.com/jbeder/yaml-cpp/releases/tag/yaml-cpp-0.9.0)、[ORT 1.32](https://github.com/microsoft/onnxruntime/releases/tag/v1.32.0)、[oneTBB 2023.2](https://github.com/uxlfoundation/oneTBB/releases/tag/v2023.2.0)、[KISS v1.0.2](https://github.com/MIT-SPARK/KISS-Matcher/releases/tag/v1.0.2)、[small_gicp tags](https://github.com/koide3/small_gicp/tags)、[ROBIN](https://github.com/MIT-SPARK/ROBIN/releases/tag/v.1.2.7)、[xenium](https://github.com/mpoeter/xenium/releases/tag/v0.0.2)。

## 稳定来源中的易错点

Boost 的 GitHub `/releases/latest` 在核对时指向 `boost-1.93.0.beta1`，其 API 甚至标记 prerelease=false。按实际版本名称和稳定发布说明，仍应选择1.92.0，不能机械地升级 Latest。

KISS 的现有8b58提交比v1.0.2领先4个提交，变更仅 README。[官方比较](https://github.com/MIT-SPARK/KISS-Matcher/compare/v1.0.2...8b58b118e00c7a5a22448741f8d956d638a44ae2)证明本次是固定来源整理，不是算法精度升级。Windows、线程隔离、数值检查和不带 PMC 的本地安全补丁保持原内容，并在干净v1.0.2源码上重新应用校验。

small_gicp 的 GitHub latest release 仍为v1.0.0，但已有非预发布命名 tag v1.0.1。现有fa0cfc比该tag领先6个提交，含初始旋转正交化、VGICP对应距离设置等修复。[官方比较](https://github.com/koide3/small_gicp/compare/v1.0.1...fa0cfc983417c044b6036f71dfa448038bb9c820)说明切回标签会退回代码，故保留完整SHA，不把开发提交冒称为新的正式release。xenium最新release年代久远，同理不为了标签倒退已验证源码。

## 固定归档

| 来源 | 完整修订/资产 | 字节数 | SHA256 |
|---|---|---:|---|
| ORT Windows x64 CPU | onnxruntime-win-x64-1.32.0.zip | 96945131 | aaa1d04fcf86bd31e84b90bc0cbde80e632bd0720dcc5d8ba05c4fe45c09d127 |
| KISS v1.0.2 | e3440b63340af7414ebd0fdde04e40f02ff4e7d2 | 255035 | d6f7d44974c6be66cfeaacbc25203fad2b083065dc694ac92b51d4c8ef8a48cb |
| oneTBB v2023.2.0 | 41158a44ab3fb7ac5df7f99ef82626c87607447f | 9206000 | 6a7cf5f1873afd6efb82af3de40ae08f4898cd4fe7de5d7bf96cd24a6546cc9b |

ORT的哈希同时与官方 GitHub release asset digest 核对；KISS/TBB按官方 tag 下载后计算并固定于匹配依赖清单，准备脚本必须再核字节数、哈希和补丁。归档、准备源码和ORT安装均位于当前功能工作区，不改变 `C:/vcpkg` 或主工作区依赖。旧归档保留独立名字，不覆盖正在复用的准备源码目录。

## C++17、Windows和运行库边界

ORT 1.32源码构建要求C++20，但LyFlow使用官方预编译CPU运行库与其C++头文件。不能由源码的语言要求直接推导使用者必须升级C++标准。独立探针已按 `/std:c++17`、`/MD` 编译当前 `ml_onnx_run.cpp`、私有 `session_cache.h` 和完整现有std-ml测试，再实际加载新版DLL/API32，证明这一消费路径可用。发布仍需让include、import library、DLL来自同一固定资产，不能只换DLL或只改版本号。

实际资产包含 `onnxruntime.lib`、`onnxruntime.dll`、`onnxruntime_providers_shared.dll`、C/C++API与inline/float16辅助头、版本文件、LICENSE和ThirdPartyNotices。准备脚本检查这些文件及VERSION_NUMBER，下载缓存按固定字节数/SHA校验，删除范围仅限功能工作区自身创建的唯一暂存目录。

oneTBB 2023.2支持Windows和当前C++17用法，二进制兼容版本仍为12，继续交付tbb12.dll；[系统要求](https://github.com/uxlfoundation/oneTBB/blob/v2023.2.0/SYSTEM_REQUIREMENTS.md)、[版本头](https://github.com/uxlfoundation/oneTBB/blob/v2023.2.0/include/oneapi/tbb/version.h)。本次仍从固定源码构建，维持 `/MD`、经典MSVC OpenMP/vcomp以及现有TLS线程预算，不切换到不可再分发的LLVM调试运行库，也不引入全局omp线程设置。

新版源码增加了默认开启的 `TCM_BUILD`。LyFlow正式CMake显式关闭TCM_BUILD/TCM_TEST、TBB测试/示例/分配器代理、自动hwloc查找及IPO，避免在依赖升级时悄悄扩展运行库组件。TCM是上游可选功能，本阶段不声称提供跨运行时线程组合管理。

## vcpkg baseline

目标baseline `4702e381dbbd9b54d3f7e1eb565cd8981c1814cd` 的直接端口仍为Boost1.92.0、Eigen5.0.1、yaml-cpp0.9.0#1；TBB为2023.2.0，ORT端口为1.31.1#2而非官方已发布的1.32.0。ORT继续使用独立官方资产，不通过修改vcpkg端口假装取得最新版。

该baseline还将传递依赖LZ4从1.10.0更新到1.11.1，FLANN端口保持2022-10-28。完整依赖树必须重新准备并核固定工具链/ABI缓存，不能只改显式列出的几个版本。这里只使用LZ4运行库，不开启其GPL工具特性。[固定端口目录](https://github.com/microsoft/vcpkg/tree/4702e381dbbd9b54d3f7e1eb565cd8981c1814cd/ports)。

## 已执行验证

独立ORT探针按MSVC19.51/C++17实际构建当前生产算子、当前私有helper和现有std-ml测试，使用仓库生成的ONNX夹具。10组现有测试加1组精确运行库/API检查，共11组146断言全过；没有跳过整个测试组。旧路径符号链接分支仍因当前进程无创建符号链接权限而输出原有说明，不把这一分支写作已验证。此证据不代替固定14.44发布工具链的CI、完整运行库发包、真实YOLO8/11/26模型与跨接口回归。

匹配新组合使用独立准备目录和正式 `matching-deps.cmake` 完成61个构建单元。实跑显示TBB头文件与实际加载的运行库均为2023.2.0，interface=12200，保持tbb12。相同毫米级双物体+500背景探针得到6792→3646→500点，两实例覆盖均1.0、RMS约34.51/34.27µm、平移误差约0.753/1.137µm、旋转误差5.33e-5/1.70e-5rad，结果与升级前探针一致，不把此软件夹具写作现场精度改善。未接受剩余背景。

没有占用或替换主构建目录正在使用的匹配源码。统一运行库构建、CLI/C ABI/SDK、YOLO和发布包的最终结果由本阶段主门禁另行记录。

## 发布与CI接入

Windows vcpkg组合缓存更新到std-glue-gap-v3和新baseline，正式OpenCV从固定源码单独构建并缓存安装前缀，不混用vcpkg OpenCV4。OpenCV缓存键含平台、固定MSVC/SDK及vision清单/脚本；没有宽泛回退。prepare-release-deps的SkipVisionBuild只跳过编译，仍运行源码、配置、必需模块和全部许可证验证；workflow仅在精确安装缓存命中时启用。依赖预热使用现有Windows release的rebuild_deps/dependencies_only入口，没有新增独立warm工作流。

安装前缀必须带正式构建生成的 `share/lyflow-opencv/build-record.json`、实际CMakeCache和编译器快照。记录绑定固定OpenCV修订/归档、完整实际配置、x64、动态CRT、构建配置和生产编译器；Windows发布复用还要求14.44/固定SDK，Linux复用要求当前C/C++编译器路径、版本和x64目标与生产时一致。release清单的版本/修订同时与vision清单及实际安装头文件核对。缺记录、旧工具集或配置不符直接拒绝，不从缓存键推定来源，也不为旧产物补写记录。只在真正构建和全部检查完成后写入记录与发布进程环境。

固定profile显式覆盖C/C++通用及当前配置的编译/链接选项、关闭IPO，并拒绝环境注入编译选项，防止旧producer缓存中的AVX、LTO、静态CRT或sanitizer设置悄悄进入SSE2运行库。`tests/release/test-build-vision-deps.ps1`直接调用生产校验函数，使用最小安装树/真实格式的CMake快照覆盖19个Windows场景，包括14.51开发产物可复用但不能满足14.44发布门禁，以及缺记录、修订/归档/配置/编译器不符、隐藏flags、版本头和许可证错误。Linux CI运行同一脚本的平台适用场景；不会为此再次编译OpenCV。公共源码准备器也纳入OpenCV精确缓存键，协议变更后重新构建。

Linux vision CI使用PowerShell7调用同一正式构建脚本，显式传入OpenCV_DIR，并将共享库目录传入CTest环境。此为工作流实现，Windows本地静态检查不能证明Linux runner已经通过；最终证据应来自该源码修订的CI。

运行库/SDK复制完整OpenCV主许可证和安装树第三方许可证（JPEG、PNG、zlib、SoftFloat、MSCR表、Annoy、DLPack、FlatBuffers等）；STag只带MIT字典来源与使用说明，排除上游ED/LSD实现。依赖inventory记录vision-opencv版本/修订及vision-stag字典修订。输出目录避开受管视觉构建/源码/安装目录，进程环境中的OpenCV路径在成功或失败后恢复。

core原始单头依赖的完整许可和来源表另放入 `licenses/core/`：nlohmann的LICENSE.MIT、doctest的LICENSE.txt、xxHash的LICENSE以及SOURCES.md。版本从实际头文件宏读取并纳入依赖清单；doctest明确标记test-only，不暗示运行库链接测试框架。官方标签/源码修订/文件SHA见[core来源表](https://github.com/liangyuR/lyflow-engine/blob/main/core/third_party/README.md)，这里的来源校验不建立产品身份。

xxHash升级另由主验证比较原0.8.2与新0.8.4的108组长短输入、内存对齐和seed向量，XXH3-128结果全部一致；没有仅凭版本说明假定缓存键协议保持不变。完整native与SDK门禁仍按本阶段实际执行记录。

当前ORT安装缓存的复用检查仍以必需文件和VERSION_NUMBER为基础，不逐文件重新验证已解压DLL来源；下载归档才做固定字节数/SHA检查。完整打包的实际DLL检查另查询ORT版本/加载目录。这是保留的缓存校验边界，没有把缓存命中写成新DLL已再次完成来源认证。

完整Windows本地集成已执行：发布七包组合的564组原生测试/137237断言全部通过，0跳过；包含四个实际检测模型、六例保存重载/参数隔离的scheduler+C ABI+SDK流程。编辑器94项及React18/19类型检查通过；安装后独立头文件SDK消费六例通过，移除geometry DLL后即使CWD/PATH有有效诱饵仍拒绝。该本地集成使用开发MSVC19.51，正式14.44/new-vcpkg-baseline及Linux构建以本PR最终CI为准；不把独立依赖probe替代发布工具链验证。

Codex审查修复后，OpenCV缓存跳过参数贯穿package→core→vision且继续执行受管校验，永久校验累计23场景（19项安装profile+4项实际调用透传）；形状有效邻域明确排除2个工作像素。并行FasterPFH的packed-bit有效标记改为独立字节，固定补丁修订3与2049点串并行语义回归见[匹配分析](p2-analysis-matching.md)，未改匹配预设或质量门限。

后续Windows完整链已通过103/103固定依赖恢复、23项profile与编译，真实失败位于既有匹配例。定位后的数值驻点资格与完整体素分组修复见同一匹配分析；KISS补丁修订至4，仍使用正式v1.0.2归档和原质量门限，不以重跑CI或选择幸运随机种子代替修复。

修复后的最终来源重新准备并验证，完整七包本地回归568组/137522断言、0跳过；三档匹配各40次均通过，隔离安装SDK六例及缺库拒绝反例通过。编辑器代码与此前94项/React18和19验证对应源码相同；新提交的正式14.44、Linux与完整CI仍须重新通过后才能合并。

# core —— C++ 核心

算子注册表、算子描述导出、图的校验/编译/执行、结果仓。三层里唯一做**计算**和
**权威校验**的一层（[docs/architecture.md](../docs/architecture.md)）。

```
include/lyflow/       公共头。零 PCL（ADR-0005），算子作者只需要看这里
  data.h              PointCloud / Indices / Transform / Plane
  operator.h          ParamView / Inputs / Outputs / ExecContext / ComputeFn
  manifest.h          算子描述的数据结构，序列化成 operator-manifest.json
  status.h            结构化诊断（paramPath / portName）
  c_api.h             C ABI v5 —— DLL 只导出这里的东西
src/exec/             parse → expand → validate → compile(Plan) → execute + ResultStore
  subgraph.cpp        子图展开成平图（ADR-0010）；library.cpp 扫描库目录
src/ops/              手写算子。**不许 include 任何 PCL 头**
src/ops/pcl/          PCL 算子，经 adapter 进出；单独吃一个 PCH
third_party/          vendored 单头：nlohmann/json、doctest、xxhash
tests/                doctest 测试
```

## 依赖

- **PCL**（vcpkg，`x64-windows` 动态三元组）：`common io filters kdtree search
  segmentation sample_consensus features`。约定装在 `C:\vcpkg`，`VCPKG_ROOT` 可覆盖。
- **vendored 单头**（不走 vcpkg，见 D7）：`nlohmann/json`（GraphDoc 解析）、
  `doctest`（测试）、`xxhash`（cacheKey）。这三样进 vcpkg 会让 core 自身的
  构建从秒级变成分钟级，而它们各自只有一个头文件。

M0/M1 时期这里写着「零第三方依赖是有意的」。M2 引入 PCL 之后那句话不再成立，
但它保护的东西还在：**「加一个手写算子」的反馈循环仍然是秒级的** ——
PCL 的重量被关在 `src/ops/pcl/` 和它专属的 PCH 里（[ADR-0005](../docs/adr/0005-pcl-boundary.md)）。

## 构建

core 是 **CMake 构建的 DLL**（[ADR-0004](../docs/adr/0004-core-as-dll.md)），
只导出 `c_api.h` 里那些 C 函数。Rust 侧用 libloading 在**运行时**加载它，
既不链 import 库，也不再用 cc crate 逐个编 `.cpp`。
`bridge/build.rs` 调的就是这份 `CMakeLists.txt` —— 两条路径同源，
不会出现「命令行能编、cargo 编不了」。

```powershell
# 构建 + 算子自检 + 跑 doctest 测试
pnpm core:build
# 或者直接
powershell -File scripts/build-core.ps1
powershell -File scripts/build-core.ps1 RelWithDebInfo -NoTests   # 只构建
```

产物全部落在 `build/core/bin/`：

| 文件 | 说明 |
|---|---|
| `lyflow_core.dll` | 唯一的交付物，只导出 C ABI |
| `lyflow-dump-manifest.exe` | 导出 manifest；`--check` 跑注册表自检 |
| `lyflow-core-tests.exe` | doctest。直接跑就行，无参数 |
| `pcl_*.dll` / `boost_*.dll` / … | vcpkg 的 applocal 自动拷来的运行时依赖 |

五条硬约束：

- **D8：固定 RelWithDebInfo**，不跟 cargo 的 profile 联动。Rust 永远用 `/MD`，
  vcpkg 的 debug 库是 `/MDd`，混用会在完全无关的地方崩。
- **D9：两个 exe 都内嵌 `activeCodePage=UTF-8` 的 manifest**（`lyflow-utf8.manifest`）。
  只有 exe 的 manifest 决定进程 ACP，给 DLL 贴无效。
- `/EHsc` 显式写在编译选项里。cmake crate 会整个替换 `CMAKE_CXX_FLAGS`，
  把 CMake 默认带的那个顶掉，后果是 MSVC 静默进入「异常关闭」模式，
  而 `c_api.cpp` 的每个入口都依赖 try/catch 兜住异常、绝不让它跨 ABI。
- `/utf-8` 同理。源文件里有中文字面量，少了它 MSVC 按系统代码页解释，
  而症状要到前端看见 manifest 里的乱码才暴露。
- **PCL 的导出目标名两种都认。** 上游 `PCLConfig.cmake` 造的是非命名空间的
  `pcl_common` / `pcl_io`，某些发行版（以及 PCL 未来的版本）用 `PCL::common`。
  硬写一种就等着在别人机器上炸；两种都找不到时退回经典的
  `${PCL_LIBRARIES}` + `${PCL_INCLUDE_DIRS}`。

所有产物（DLL、exe、vcpkg applocal 拷来的依赖 DLL）都落在同一个 `bin/`，
`bridge/build.rs` 整目录拷贝它 —— Rust 侧运行时才能 `LoadLibrary` 到
`lyflow_core.dll` 以及它依赖的那几十个 PCL/boost DLL。

手工调 CMake 时记得先进 MSVC 环境（`vcvars64.bat`）并传 vcpkg 工具链：

```bash
cmake -S core -B build/core -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/core
```

## 加一个算子

1. 新建 `src/ops/<category>_<name>.cpp`（要用 PCL 就放 `src/ops/pcl/` 下），
   实现 `Status compute(...)` 和 `void registerXxx(Registry&)`
2. 在 `src/ops/ops.h` 加声明
3. 在 `src/builtin_ops.cpp` 的 `registerBuiltinOps` 里加一行调用

**前端不用改任何东西。** 这是 [ADR-0003](../docs/adr/0003-manifest-from-cpp.md) 的承诺。

> 为什么不用静态对象自注册（那样只需改第 1 步）？链接器会丢掉没有符号被引用的
> .obj，自注册的算子会静默消失 —— 而且往往只在 release 构建里消失。绕开要靠
> `/WHOLEARCHIVE` 之类的链接器开关，等于把正确性押在构建配置上。
> 多写一行调用换确定性，划算。

写 compute 时的四条约定：

- **参数不用校验。** `ParamView` 拿到的值已经过执行器校验并合并了默认值（D6）。
  算子只需要报两类错：跨参数的语义约束（带 `paramPath`）、IO/输入内容问题
  （带 `portName` 或 `paramPath`）。
- **滤波类算子必须经 `PointCloud::select` 出结果**，否则 intensity/normals/rgb
  会被悄悄丢掉，而症状是「下游的着色突然没了」。
- **长循环里用 `ops::Ticker` 轮询取消**，并在 `capabilities.cancellable` 里如实申报。
  PCL 的算法一旦进去就出不来，这类算子要填 `false`。
- **自己开线程时问 `ctx.threadBudget()`**，别按核数开。执行器已经在同时跑
  `maxParallel` 个节点，算子内部再按核数开一遍就是超订，比串行还慢。

## 并行

执行器是**依赖计数驱动的线程池**（E2，没有层同步屏障 —— 屏障会让一层里最慢的
节点拖住全部）。`maxParallel` 由 run 选项给，默认 `min(4, 硬件线程数)`；
就绪队列按 `level` 再按拓扑序排，所以同样一张图两次跑出来的调度是稳定的。

三条要点：

- `EventSink` 整个入口加锁，`seq` 仍然全局单调。前端靠 seq 检测丢包，
  并行下最容易坏的就是它。
- 取消是所有 worker 共用的一个 `atomic<bool>`。每个节点在开跑前和算完后各查一次，
  加上 `ops::Ticker` 在循环里查 —— 所以取消的响应时间是「最慢的那个算子的一次轮询」。
- **`ctx.threadBudget()` = max(1, cores / maxParallel)**。PCL 的 OMP 版本算子
  （`NormalEstimationOMP` 这类）应当把它传给 `setNumberOfThreads`。

## 子图与库算子

`sub:<id>` 与 `lib.<id>` 两种节点在 **compile 之前**被 `expandGraph` 递归替换成平图
（[ADR-0010](../docs/adr/0010-subgraph-by-expansion.md)）。执行器、结果仓、事件流
一行都没为它们改过 —— 它们看见的永远是一张平图。

- 展开后的 id 是路径 `outer/inner/leaf`；`Run to node` 的目标按**路径前缀**匹配，
  所以给一个子图节点的 id 等于给它展开后的全部内部节点。
- 提升参数（`params[].binds`）在展开时把外参的值写进内参，覆盖内参自己的值。
- 静音一个子图节点 = 整棵子树都静音，每个内部节点各自按 E5 透传。
- 递归引用报 `recursive_subgraph`，嵌套深度上限 32，两者都是整图级失败。
- 库算子由 `lyflow_set_library_dirs` 扫描 `*.lyflow-op.json` 注册成 `lib.<id>`，
  分类前缀 `Library/`。合成出来的 `OperatorDesc` 单独过一遍 `Registry::validate()`，
  坏的那一个被跳过而不牵连其余。**调用它之前必须放掉所有 run**：
  它会重建注册表的后半段，旧的 `OperatorDesc*` 随之失效（与热重载同一条约定）。

## live preview

`RunOptions::mode == Preview` 时，**没有输入边**的节点在输出进结果仓之前先等步长抽稀到
`previewMaxPoints`（默认 20 万），其余算子一行都不改
（[ADR-0011](../docs/adr/0011-preview-as-decimated-run.md)）。
抽稀走 `PointCloud::select`，所有通道一起搬。

cacheKey 混入 `preview:<maxPoints>`，所以预览与正式的结果互不命中；
超过 `previewBudgetMs`（默认 300）时发一条 `log warn`。

## 缓存

结果仓按 cacheKey 内容寻址，**判定只在这里**（[ADR-0007](../docs/adr/0007-cache-authority.md)）。
命中的节点直接把仓里的 `shared_ptr` 挂到输出，发 `node_state: skipped` + `stats.cached`，
不调 compute。

- 生命周期由 **LRU 字节预算**管，默认 `min(8 GB, 物理内存 40%)`，
  `lyflow_run_options.cache_budget_bytes` 可覆盖。`freeRun` 只丢索引不删数据。
- 一次运行开始时用 `CachePin` 把计划里所有键钉住，防止上游结果在下游读到之前被淘汰。
- **没有输出端口的算子永远不复用**（`io.save_pcd` 这类纯副作用的）。
  跳过它的表现是「跑成功了但文件没写出来」。
- `bypass` 进 cacheKey：静音改变结果本身，不进键的话取消静音会拿到旧结果。

`lyflow_plan` 把每节点的 `{ cacheKey, cached, level, upstreamMissing, bypass }` 报给前端，
`lyflow_cache_stats` / `lyflow_cache_clear` 给状态栏和菜单用。
预览的那一份用的是另一组键，两边在同一个 LRU 里但永远不会互相命中。

## 算子改版本

主版本升级要配一条迁移（[ADR-0008](../docs/adr/0008-migration-as-diagnostic.md)）：

```cpp
nlohmann::json migrateFromV1(const nlohmann::json& params) { /* 改写参数 */ }
...
op.version = "2.0.0";
op.migrations = { Migration{1, &migrateFromV1} };
```

`Registry::validate()` 要求链条覆盖 `1..currentMajor-1` 且无断档 —— 缺一环启动就报。
改 id 用 `op.aliases`，老 id 会被自动重定向，同样产出一条迁移诊断。
迁移只碰参数：端口增删改名要靠新算子 id + aliases，因为连线不归算子管。

现成的例子是 `filter.random_sample` 的 1.0.0 → 2.0.0（`count`/`ratio` →
`keepCount`/`keepRatio`）。

## 自检与测试

`Registry::validate()` 在导出前检查算子描述的自洽性：端口类型是否存在、
id 是否重复、参数默认值与声明类型是否匹配、enum 默认值是否在选项里、
参数联动是否引用了不存在的参数、**compute 是否为空**。

有问题时 `lyflow-dump-manifest --check` 退出码非 0，桥接层启动时也会当作 fatal。
理由：契约破了还继续跑，前端会拿到一份自相矛盾的 manifest 然后用各种
离奇的方式崩掉，排查成本远高于启动时直接报错。

`Registry::validate()` 还检查迁移链是否覆盖 `1..currentMajor-1`。

`tests/` 是 doctest，覆盖：拓扑与环、诊断全量返回且 paramPath 正确、默认值合并、
类型兼容矩阵、取消在第 k 个节点生效、上游失败传播、`select` 保留全部通道、
结果仓的抽样与 bounds、中文路径下的 PCD 往返，以及 M3 的
缓存命中/LRU 淘汰/plan 预测一致性、并行菱形图与随机取消 100 次、
bypass 透传与 `bypassed_no_source`、`Any` 推导、迁移链与隐藏参数。
点云一律由 `gen.synthetic` 现生成，**仓库里不放二进制样例数据**。

> `test::Session` 默认在构造时清空结果仓。缓存是进程级的，不清的话
> 「第二个用同一张图的测试」会拿到 `skipped` 而不是 `done` —— 那是真实行为，
> 但会让断言测的是运行顺序而不是被测的那件事。要验缓存就用 `runGraphCached`。

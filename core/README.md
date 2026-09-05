# core —— C++ 核心

算子注册表、算子描述导出、图的校验/编译/执行、结果仓。三层里唯一做**计算**和
**权威校验**的一层（[docs/architecture.md](../docs/architecture.md)）。

```
include/lyflow/       公共头。零 PCL（ADR-0005），算子作者只需要看这里
  data.h              PointCloud / Indices / Transform / Plane
  operator.h          ParamView / Inputs / Outputs / ExecContext / ComputeFn
  manifest.h          算子描述的数据结构，序列化成 operator-manifest.json
  status.h            结构化诊断（paramPath / portName）
  c_api.h             C ABI v2 —— DLL 只导出这里的东西
src/exec/             parse → validate → compile(Plan) → execute + ResultStore
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

三条硬约束：

- **D8：固定 RelWithDebInfo**，不跟 cargo 的 profile 联动。Rust 永远用 `/MD`，
  vcpkg 的 debug 库是 `/MDd`，混用会在完全无关的地方崩。
- **D9：两个 exe 都内嵌 `activeCodePage=UTF-8` 的 manifest**（`lyflow-utf8.manifest`）。
  只有 exe 的 manifest 决定进程 ACP，给 DLL 贴无效。
- `/EHsc` 显式写在编译选项里。cmake crate 会整个替换 `CMAKE_CXX_FLAGS`，
  把 CMake 默认带的那个顶掉，而 `c_api.cpp` 的每个入口都依赖 try/catch。

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

写 compute 时的三条约定：

- **参数不用校验。** `ParamView` 拿到的值已经过执行器校验并合并了默认值（D6）。
  算子只需要报两类错：跨参数的语义约束（带 `paramPath`）、IO/输入内容问题
  （带 `portName` 或 `paramPath`）。
- **滤波类算子必须经 `PointCloud::select` 出结果**，否则 intensity/normals/rgb
  会被悄悄丢掉，而症状是「下游的着色突然没了」。
- **长循环里用 `ops::Ticker` 轮询取消**，并在 `capabilities.cancellable` 里如实申报。
  PCL 的算法一旦进去就出不来，这类算子要填 `false`。

## 自检与测试

`Registry::validate()` 在导出前检查算子描述的自洽性：端口类型是否存在、
id 是否重复、参数默认值与声明类型是否匹配、enum 默认值是否在选项里、
参数联动是否引用了不存在的参数、**compute 是否为空**。

有问题时 `lyflow-dump-manifest --check` 退出码非 0，桥接层启动时也会当作 fatal。
理由：契约破了还继续跑，前端会拿到一份自相矛盾的 manifest 然后用各种
离奇的方式崩掉，排查成本远高于启动时直接报错。

`tests/` 是 doctest，覆盖：拓扑与环、诊断全量返回且 paramPath 正确、默认值合并、
类型兼容矩阵、取消在第 k 个节点生效、上游失败传播、`select` 保留全部通道、
结果仓的抽样与 bounds、中文路径下的 PCD 往返。
点云一律由 `gen.synthetic` 现生成，**仓库里不放二进制样例数据**。

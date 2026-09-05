# core —— C++ 核心

算子注册表与算子描述导出。M0 阶段只有描述，没有计算实体（compute 是 M2）。

**零第三方依赖是有意的。** 这条链路的构建必须是秒级的，否则「加一个算子」的
反馈循环会被 vcpkg 拖垮。PCL 等到 M2 算子真要跑起来时再引。

## 加一个算子

1. 新建 `src/ops/<category>_<name>.cpp`，实现 `void registerXxx(Registry&)`
2. 在 `src/ops/ops.h` 加声明
3. 在 `src/builtin_ops.cpp` 的 `registerBuiltinOps` 里加一行调用

**前端不用改任何东西。** 这是 [ADR-0003](../docs/adr/0003-manifest-from-cpp.md) 的承诺。

> 为什么不用静态对象自注册（那样只需改第 1 步）？core 是静态库，链接器会丢掉
> 没有符号被引用的 .obj，自注册的算子会静默消失 —— 而且往往只在 release 构建里
> 消失。绕开要靠 `/WHOLEARCHIVE` 之类的链接器开关，等于把正确性押在构建配置上。
> 多写一行调用换确定性，划算。

## 构建

Rust 侧通过 `cc` crate 直接编译这些源文件，**不需要 CMake**。
下面的 CMake 流程是给单独调试 core 用的。

```bash
cmake -S core -B build/core -G Ninja
cmake --build build/core
./build/core/lyflow-dump-manifest --check
```

Windows 上需要先进 MSVC 环境（`vcvars64.bat`），CMake 与 Ninja 可用 VS 自带的。
仓库根目录提供了 `scripts/build-core.ps1` 代劳。

## 自检

`Registry::validate()` 在导出前检查算子描述的自洽性：端口类型是否存在、
id 是否重复、参数默认值与声明类型是否匹配、enum 默认值是否在选项里、
参数联动是否引用了不存在的参数。

有问题时 `lyflow-dump-manifest` 退出码非 0，桥接层启动时也会当作 fatal。
理由：契约破了还继续跑，前端会拿到一份自相矛盾的 manifest 然后用各种
离奇的方式崩掉，排查成本远高于启动时直接报错。

# 公共稳健轮廓拟合核

此 C++17 示例只链接安装 SDK 的 `lyflow::profile_robust_kernel`，使用显式配置的 PCL 后端，不加载 LyFlow 运行库。公开头只包含标准 C++ 类型；输入数组只借用且只读，结果索引指向原始数组槽位。

示例调用 RANSAC 直线、轴约束直线、自由半径圆、固定半径圆及软半径先验，检查结果有限、内点指向原数组有效槽、半径落在声明范围内，以及 NaN 槽、输入不变、失败和取消时输出原子性。拟合数值判据由源码树 `kernels/tests` 负责。样本、阈值和半径统一使用米；这个低层接口要求调用方先明确选择坐标平面及单位。

M4b还调用`profile_features::fitLine`完整近端重拟合与方向约束流程，检查原槽innerSlot、端点、固定方向回退的最终多重权重，以及质量拒绝和取消不覆盖已有结果；这些最终权重不是RANSAC内部求解抽样轨迹。

在已安装 MSVC、Ninja 和相同版本 PCL 的开发环境中：

```powershell
cmake -S <sdk>/examples/profile-robust -B build/profile-robust -G Ninja -DCMAKE_BUILD_TYPE=Release -Dlyflow_DIR=<sdk> -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_MANIFEST_MODE=OFF
cmake --build build/profile-robust
ctest --test-dir build/profile-robust --output-on-failure
```

PCL 及其传递运行依赖必须可加载。仓库交付门禁 `tests/release/public_robust_kernel_contract.py` 将实际 PE 依赖闭包复制到隔离目录，移除开发目录 PATH，核对不加载 `lyflow_core`、OpenCV、ONNX Runtime 或匹配后端，再运行消费者验收。四项轻量公共核另使用 `public_kernel_contract.py` 验证，在同一 SDK 中禁用 PCL 查找仍可独立消费。

# DTS 原生图切换

先确认历史业务使用的钣金基准和正号，再选择 [默认基准](../solutions/dts/graphs/default-profile.lyflow.json) 或 [相邻基准](../solutions/dts/graphs/adjacent-profile.lyflow.json)。以旧图的输入、参数和命名输出为依据，在新版搭建独立候选图。

## 输入

- 采集宿主将完整原生 Profile 注入 `profile.input` 的 `profile` 输出。由宿主声明实际的单位、坐标帧、手性和来源，同时保留采集顺序、有效性、原始索引与可用通道。[原生轮廓与嵌入示例](../examples/m3-native-profile.md)
- 文件调试用严格 CSV v1。XZ 轮廓可用 `x,z` 或 `x,z,intensity` 表头。文件数值使用毫米以外的单位时，同步更新输入节点的 `unit` 和其他实际坐标描述。

```powershell
lyflow run solutions/dts/graphs/default-profile.lyflow.json --set 'n_in.source="file"' --set 'n_in.path="C:/data/profile.csv"' --outputs --summary
lyflow run solutions/dts/graphs/adjacent-profile.lyflow.json --input 'n_in.profile=C:/data/profile.lypr' --outputs --summary
```

旧 `dts.profile_in` 点云输入已退役。使用旧文件作为需求参考时，与来源方确认 NaN、低强度和有效性掩码分别表示什么，再转换成原生轮廓或严格 CSV。

## 输出和判定

将下游直接连到 DTS 场景算子发布的米制点和线。旧 `dts.geometry_to_metric` 已退役。历史几何上的 `Meter` 标签可能曾对应毫米工作坐标，重建时应核对真实单位。

`dts.flush` 接收根部记录并输出毫米 Measurement、米制垂足和毫米垂足记录。`DtsProfileBundle.v3` 提供毫米面差和工作坐标，供宿主使用实际公差作业务判定。两种基准图分别验证选面及有效性，再核对相同设备样本上的业务结果。[DTS 结果契约](../solutions/dts/README.md)

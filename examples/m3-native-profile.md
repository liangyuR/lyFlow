# 原生轮廓源、LYPR 与嵌入示例

`m3-native-profile.lyflow.json` 只使用随附合成 CSV：`profile.input → profile.clean → profile.fit_line → metrology.profile_gap_flush`。两个拟合区间和量测参数均按米解释，CSV 坐标明确声明为毫米。预期 `gap=0.002 m`、`flush=0.002 m`；8 个源槽中 6 个有效，两个 NaN 槽保持位置。

```powershell
lyflow run examples/m3-native-profile.lyflow.json --outputs --summary
lyflow dump examples/m3-native-profile.lyflow.json source:profile build/m3-source.lypr --format profile
lyflow run examples/m3-native-profile.lyflow.json --input source.profile=build/m3-source.lypr --outputs
lyflow dump examples/m3-native-profile.lyflow.json clean:clean build/m3-clean.lypr --format profile --input source.profile=build/m3-source.lypr
```

`.lypr` 扩展名或 `LYPR` 文件头选择完整原生 Profile 输入，必须满足 `offset=0`、`totalPoints=count`。预览切片不能作为源。CLI 保留原始槽、有效性、源索引、Descriptor 与可选通道，默认每文件字节预算为 256 MiB；不推断单位、坐标帧或采样顺序。

文件源使用严格 CSV v1：所声明平面的两个坐标，可加第三列强度。首个非空、非注释行可为 `x,z,intensity` 等对应平面的精确小写表头。空行及 `#` 整行注释不计槽；缺强度补 1，NaN 坐标保槽并标无效，Inf、溢出、空字段及错误文本带物理行号失败。全部坐标声明必须显式提供，`sourceCount` 等于数据行数。`profile.clean` 的保槽模式保留无效槽原始载荷；拟合显式选择 `valid_only` 后只在工作缓冲中选择有效点。

`embed_profile.cpp` 通过安装 SDK 的 `lyflow::client` 独立编译。它向 `profile.input` 注入含 NaN 槽、有限无效槽、非连续源索引及强度/法线/RGB 的原生 Profile，释放调用方数组后运行同类清洗、拟合、米制量测链，并验证单位换算和坏输入拒绝。成功时输出一行 `lyflow.ProfileSdkAcceptance.v1` JSON。此示例无相机、PLC 或现场标定数据。

```powershell
profile_sdk_consumer.exe C:/installed-lyflow/bin/lyflow_core.dll
python tests/release/p5_sdk_contract.py --sdk C:/installed-lyflow --consumer C:/consumer/profile_sdk_consumer.exe --profile-consumer C:/consumer/profile_sdk_consumer.exe --profile-only --output-dir build/m3-profile-sdk-evidence
```

这些验收验证合成输入上的原生契约、传输和 SDK 消费行为；实际采集、标定、质量门限与现场性能需另行验收。

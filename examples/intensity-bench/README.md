# PCD 强度阈值诊断

直接在浏览器打开 `index.html`，选择或拖入本地 PCD 文件，检查强度分布和阈值筛选后的点云。
现有编辑器点云面板提供强度着色；此页用于独立的阈值探索，不需要图、应用数据库或 MCP 服务。

支持 ASCII 与未压缩 binary PCD，要求 `x/y/z` 及 `intensity` 或 `rgb` 字段；不支持 `binary_compressed`。
文件在浏览器本地解析，不上传点云。页面字体来自 Google Fonts，离线时使用系统字体回退。

来源：从 xyz-gap-inspector 的 `tools/intensity_bench.html` 迁入（将分布形状的效果断言改为需真实样本验证），业务仓库不再保留副本。

强度直方图只辅助观察分布；峰的数量不能证明阈值有效或无效，应使用真实代表性样本核对保留和删除的点。

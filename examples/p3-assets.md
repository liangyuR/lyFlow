# P3 示例资产来源

这些是可重复检查算法链路的合成夹具，不是工业相机实拍或现场验收样本。

| 文件 | 来源与几何定义 |
| --- | --- |
| `p3-marker-scene.png` | OpenCV 5.1.0 固定源码 `e4f78fbcf1a773fbed69e14500e8b734963eb585` 的 `DICT_4X4_50` ID 7；400px 图样按像素外边界投影至1280×960。K、位姿与米制边长见相机文件和 `p3-fixtures.json`，D为空。 |
| `p3-stag-hd15.png` | `ManfredStoiber/stag` 固定源码 `6214edc03095b6c488d3d95d866aa1a84228e574` 的 `ref/marker_generator/HD15.txt` 第0项，按该字典极坐标图样几何渲染；1000×1000，100px静区，约800px外框。字典与图样按上游MIT声明保留署名。只使用字典数据，未编译或复制上游ED/LSD检测器。 |
| `p3-shape-L-scene.png` | 原创平面L形CAD的确定性二值栅格，600px焦距、128×128投影中局部坐标加[90,60]px平移。 |
| `p3-shape-parallel-scene.png` | 原创L/H平面CAD栅格，分别加[60,60]与[200,120]px平移；两个物体互不重叠。 |
| `p3-cad-L.stl` / `p3-cad-H.stl` | 原创米制平面三角形集合，z=1m，法线[0,0,1]。分别4/6个三角形；这是开放平面网格，不冒充封闭实体CAD。运行时按固定seed=7和三角面积采样150000点。 |
| 相机、工作空间与真值JSON/YAML | 本仓库编写的小型合成数据。原生工作空间有显式版本、米制和帧树；外部格式样例明确无版本，必须由图声明格式和单位。 |

OpenCV和STag的完整许可文本随 `p3-asset-licenses.txt` 提供。OpenCV来源为官方5.1.0；STag字典来源的MIT署名为Burak Benligiray（2019）与Manfred Stoiber（2023）。原始STag仓库中ED/LSD等实现的许可范围不由该MIT字典声明扩张。

图文件不内嵌特征Tensor。形状模板在每次运行中由CAD采样投影重建；`p3-fixtures.json`只记录预先设定的真值，不连接任何运行节点、不注入检测结果或特征。

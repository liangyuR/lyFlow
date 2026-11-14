# B05 标注加载分析与契约

已阅读本机 `xyz-vision-lib` 的 `LoadAnnotations.cpp/.hpp`。本机来源没有Git修订证据，只分析语义并独立实现，不复制proprietary函数体。

参考实现按输入图片时间戳在 `samples_v2.txt` 的RGB文件名首13字符查找第三列标注JSON，读取width/height、roi、instances的category/bbox/parts及pickable/isoccluded，输出实例与ROI。它只保留首个polygon、空parts会越界、重复时间戳会覆盖、路径可以越出数据集、mask坐标与扩展后的bbox可能不一致；这些缺陷不保留。

本轮 `image.load_annotations` 显式读取数据集索引和时间戳，加载索引指定的对应图片与标注，消除独立传入图片时无法验证时间戳来源的问题。图片和掩膜保留原生载荷。

## 参数与输出

- `path`：数据集索引文件，UTF-8路径；其所在目录为数据集根。
- `format`：`lyflow-annotations-v1` 或 `xyz-samples-v2`，不猜测文件格式。
- `timestamp`：1..20位十进制字符串，保留精度及前导零；旧格式要求13位。
- `frame`：默认 `image`，px单位，标注若声明frame须一致。
- `pickableOnly`：仅按源标注字段筛选，默认false；`includeOccluded` 默认true。
- 输出 `image` 为对应源图，`overlay` 保留源位深/通道/alpha；`instances` 和 `rois` 使用标准 `vision.Poses2D.v1`。
- 输出 `masks` 为float32原生Tensor `[N,H,W]`，0/1，全图坐标，可保留重叠。空结果为 `[0,H,W]` 和空data。
- `maskInfo` 为 `vision.Masks2D.v1`：`frame/unit/width/height/layout/items`，layout为NHW，items按实例顺序保存id/index/hasMask/source。hasMask由实际非零光栅支持判定；没有polygon或被holes完整扣除的实例保留其bbox/ID和全零层，但hasMask=false、source=none，Pose.attributes同步。不能仅凭parts非空宣称存在分割，也不把bbox伪装成分割。此类实例接分割keep模式时按缺mask继续推理。
- `roiMask` 为u8全图并集；未声明ROI时为全零并输出空rois，质量中明确未提供，不能通过连空ROI触发全图fallback。
- `quality` 记录选中时间戳、源相对路径、尺寸、输入/保留/过滤实例数、掩膜和ROI状态。

所有交付实例 `pickable=false`。源标注pickable与occluded保留在attributes并可用于筛选，不自动授予抓取许可。

## 原生格式

索引：

```json
{
  "schemaVersion": 1,
  "samples": [
    {"timestamp": "1700000000000", "image": "images/工件.png", "annotation": "labels/工件.json"}
  ]
}
```

标注：

```json
{
  "schemaVersion": 1,
  "width": 64,
  "height": 48,
  "frame": "image",
  "rois": [
    {"id": "working-area", "polygon": [[0,0],[63,0],[63,47],[0,47]], "holes": []}
  ],
  "instances": [
    {
      "id": "part-1",
      "label": "连接件",
      "classId": 1,
      "bbox": [8,8,24,20],
      "confidence": 1,
      "pickable": false,
      "occluded": false,
      "parts": [
        {"polygon": [[8,8],[31,8],[31,27],[8,27]], "holes": []}
      ]
    }
  ]
}
```

bbox为原图px的xywh；polygon为同一原图坐标，多个part取并集，每part的holes仅减去该part。重叠part不因另一part的洞被误删。交付bbox由标注框与实际非零支持范围的并集形成，范围限制在图片内。光栅化使用OpenCV包含边界的多边形填充规则，不通过JSON交换像素。

每part的临时范围覆盖outer和全部holes的完整固定点光栅范围，可能包含图像边界外由连续顶点四舍五入产生的一圈支持。先按扩展后的真实临时范围计入光栅预算，再验证每个hole的完整像素支持属于outer，最后才裁到原图并合并。不能先将越过outer缓存的hole剪空再把它认作合法；合法贴边和小数顶点采用同一填充规则，不以过严的连续坐标包围框判断代替像素包含关系。

## 显式旧格式

索引为 `samples_v2.txt`，前三列分别RGB、depth、annotation，其余列不消费。支持quoted comma和双引号转义，拒绝嵌入换行的字段。RGB basename的首13字符必须为十进制时间戳；相同时间戳不允许重复。depth列不作为本算子的深度数据读取承诺。

标注使用已读字段：width/height、可选roi polygon，以及instances中的instance_category、bbox、全部parts[*].polygon、pickable/isoccluded的0/1。实例未提供id时按源数组索引生成稳定id，不能仅按筛选后的顺序重编号。保留全部有效part；不复制参考实现只读首part或越界行为。未核实的外部请求/工作空间协议仍不宣称支持。

## 安全边界与验证

索引和标注各最多4MiB，图片压缩文件最多32MiB、解码后最多8Mi像素；实例最多512、polygon总顶点最多100000。`N*H*W`最多32Mi float32元素，光栅工作预算额外限定，防止大量重叠part耗尽时间。JSON深度与字符串、有限坐标和正尺寸都校验；解码图的真实尺寸必须等于标注。OpenCV内部解码峰值不作为硬内存隔离保证。

所有数据路径必须为数据集内相对路径，拒绝绝对路径、驱动器、`..`、控制字符及符号链接越界。仅访问选中样本的图片/标注；重复时间戳、缺时间戳、缺文件、坏格式与坏尺寸产生具体诊断。无隐式缺图补零或忽略坏实例。

节点关闭结果缓存，每次重读文件；计算中检查取消，全部输出完成并通过结构校验后一次提交，失败不遗留半套掩膜或污染源数据。验收包含Unicode路径/标签、多part/洞/重叠、缺mask、筛选、空实例/ROI、同尺寸文件更新、重复/缺时间戳、路径逃逸、资源限额及真实保存重载/CLI/C ABI/SDK链。

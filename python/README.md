# LyFlow Python 节点 SDK

运行环境要求 Python 3.11+ 和 NumPy。解释器及依赖由使用者管理；LyFlow 不自动安装依赖、不执行图文件里的内联 Python。节点来自本机 `.py` 文件或包目录，导入模块会执行其中的 Python 代码，只加载可信来源。

worker 在发现或执行任务前检查解释器版本；低于 Python 3.11 时返回 `python_version_unsupported`，不会导入节点。

SDK 目录是本文件所在的 `python/`，模块是 `lyflow_python`。原生运行时通过绝对路径启动 `lyflow_python/worker.py`，节点中可以直接 `from lyflow_python import ...`，不需要把 SDK 安装进选中的解释器。独立运行示例或测试时，将 SDK 目录加入 `PYTHONPATH`。

## 定义节点

```python
import numpy as np
from lyflow_python import node

@node(
    id="py.example.offset",
    version="1.0.0",
    label="点云平移",
    category="Python/点云",
    inputs={"cloud": "PointCloud"},
    outputs={"cloud": "PointCloud"},
    params={"offset": {"type": "float", "default": 0.01, "unit": "m"}},
)
def offset(inputs, params, ctx):
    result = inputs["cloud"].copy()
    result.xyz[:, 2] += np.float32(params["offset"])
    ctx.progress(1.0, "完成")
    return {"cloud": result}
```

`id` 使用 `py.` 命名空间，建议 `py.<包名>.<节点名>`。版本写语义版本，如 `1.0.0`。参数规格沿用 LyFlow manifest 的 `type`、`default`、`label`、`min/max`、`unit`、`options`、分组与联动字段；运行时补齐默认值并检查参数类型和硬范围。函数接收 `inputs`、`params` 两个字典及 `ctx` 上下文；输出返回字典，键必须与所有声明输出完全一致。SDK 也兼容 `def offset(ctx, cloud, offset)` 这类按名称接收关键字参数的函数。端口与参数名不能重叠或占用 `ctx`。

`enum` 的执行值始终是字符串。声明中的数字 `default` 和选项 `value` 会转换为字符串，例如 `default: 2`、`options: [{"value": 1, "label": "低"}, {"value": 2, "label": "高", "doc": "高精度"}]` 对应函数收到 `"2"`；显式参数也传 `"1"` 等字符串。选项支持 `value`、`label`、`doc`，省略 `label` 时使用规范后的值；布尔值和非有限数不能作为枚举值。

端口可以简写为类型字符串，也可写 `{"type": "PointCloud", "label": "输入", "required": false}` 等完整规格。可选输入未连接时不出现在 `inputs` 字典中，节点可用 `inputs.get("name")` 处理；关键字兼容签名需提供对应默认参数。函数必须同步执行；异步协程、额外必填参数与未声明端口会给出明确错误。

发现方式：

- 单个 `.py` 文件：查找其中导出的装饰函数。
- 有 `__init__.py` 的包：加载包入口，支持 `from .nodes import offset` 导出函数。

不支持容器目录扫描或递归查找。没有 `__init__.py` 的目录会报错；请在 `packagePaths` 中分别列出脚本和包目录。包内只发现入口导出的装饰函数，未导出的实现文件不会另行扫描。

坏文件、缺第三方依赖的包会分别记录路径、错误和完整 traceback，其余包继续发现。重复算子 ID 的全部冲突项禁用，避免加载顺序决定实际实现。发现成功返回的每个算子强制使用 `deterministic: false`、`cancellable: true`、`previewable: false`；Python 节点不复用原生确定性缓存或抽稀预览。

## 数据与生命周期

| SDK 类型 | 内容 |
| --- | --- |
| `PointCloud(xyz, intensity=None, normals=None, rgb=None)` | `xyz` 为 `N×3 float32`；强度 `N float32`、法线 `N×3 float32`、RGB `N×3 uint8` 可选。允许零点云。 |
| `Image(pixels)` | `H×W` 或 `H×W×C`，`C` 为 1、3、4；位深 `uint8`、`uint16`、`float32`，通道按 RGB/RGBA 排列。 |
| `Measurement(value, unit="", ok=None, ...)` | `value` 为有限实数或 `None`；没测出时 `ok=False`。可带 `message`、`verdict`，以及同时提供的 `nominal/lower/upper`。 |
| `Record(data, record_type="")` | JSON 数据和记录类型；`record_type` 在协议中写成 `recordType`。 |

输入数组通过只读 `np.memmap` 打开，输入对象及 Record 的嵌套数据也不可修改。需要编辑时调用 `.copy()`；输出可以是副本或新建的 SDK 数据对象。数组位深和形状必须显式匹配，SDK 不悄悄把 float64 点云转成 float32。输出非连续数组会按连续小端布局写出。

映射只在本次调用期间有效，不要把输入数组或其视图留在模块全局、后台线程中继续访问。worker 在成功、异常、取消路径都会关闭输入映射，返回前也关闭输出文件句柄。失败或取消时移除本次生成的输出文件；原生运行时负责请求目录的最终清理。

## 上下文

- `ctx.log(message, level="info")`：结构化日志，级别 `debug/info/warn/error`。
- `ctx.progress(ratio, message="")`：0 到 1 的有限进度。
- `ctx.cancelled`、`ctx.check_cancelled()`：合作取消。长循环应按块检查，后者抛 `NodeCancelled`。
- `ctx.base_dir`：图所在目录的 `Path`；相对业务文件可用 `ctx.base_dir / relative_path`，worker 不改变全局工作目录。
- `ctx.thread_budget`：本节点可用的线程预算，由节点库据此配置自己的并行工作量。

Python 函数中的 `print`、原生扩展输出与 stderr 仍是日志，不参与协议。取消消息由主线程持续读取；函数在独立任务线程运行，任务串行执行。阻塞且不检查取消的第三方函数不能被 Python 安全中断，父进程会按运行时期限终止 worker。

## worker 协议

```text
python -u -B <sdkDir>/lyflow_python/worker.py --pipe <Win32命名管道名>
```

管道为独立双工 byte 模式，每帧为 4 字节 little-endian 字节长度和 UTF-8 JSON。单个 JSON 帧上限 16 MiB；点云与图像正文使用请求目录内的 `.bin` 文件，不放进 JSON。

请求与响应：

```json
{"id":"d1","command":"discover","packagePaths":["C:/nodes/offset.py"]}
{"id":"d1","event":"result","operators":[],"problems":[],"pythonVersion":"3.11.9","numpyVersion":"2.2.6"}
```

```json
{"id":"e1","command":"execute","operatorId":"py.example.offset","inputs":{"cloud":{"type":"PointCloud","xyz":{"file":"C:/request/in.bin","shape":[2,3],"dtype":"f32"}}},"params":{"offset":0.01},"baseDir":"C:/graphs","threadBudget":2,"requestDir":"C:/request"}
{"id":"e1","event":"progress","ratio":1.0,"message":"完成"}
{"id":"e1","event":"result","outputs":{"cloud":{"type":"PointCloud","xyz":{"file":"C:/request/out-unique.bin","shape":[2,3],"dtype":"f32"}}}}
```

数组 DTO 仅有 `file/shape/dtype`，`dtype` 为 `f32/u8/u16`。输入文件必须是请求目录内的绝对路径；检查解析后的路径、形状和精确字节数。JSON 类型分别为 `{"type":"Measurement","value":1.2,"unit":"mm","ok":true}` 和 `{"type":"Record","recordType":"example.info","data":{}}`。

取消请求 `{"id":"e1","command":"cancel"}` 使用正在执行任务的相同 ID，合作终止后响应 `event: "cancelled"`。异常帧含 `event: "error"`、`code`、`message`、`traceback`；区分未发现算子、依赖缺失、输入非法、输出非法与 Python 函数异常。响应以 `result/error/cancelled` 之一结束本次请求。

结束响应发送前关闭本次请求的全部数组映射，失败时删除已生成的输出文件。包导入和函数中的 `SystemExit`、`KeyboardInterrupt` 转为诊断，后续任务仍可执行。

## 示例和测试

`examples/offset.py` 是使用 `offset` 参数沿 Z 轴平移、带分块取消的点云节点；`examples/image_metrics/` 展示包入口导出、图像处理、量测和 Record 输出。

在仓库根目录运行：

```powershell
python -B -m unittest discover -s python/tests -v
```

测试覆盖装饰器、发现与坏包/重复 ID、数据形状与只读副本、各种位深和通道、路径越界和文件长度、短读短写帧、完整 traceback、串行队列、取消和映射释放。帧与取消测试使用内存双工字节适配器，不需要监听端口；真实 Windows 命名管道、原生图执行、超时杀进程由 core 集成验收覆盖。

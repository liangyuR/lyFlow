# Python 自定义节点

在「库 → Python 节点」选择 Python 解释器，添加 `.py` 文件或包含 `__init__.py` 的包目录，然后保存并检查环境。最低 Python 3.11，所选环境需要安装 NumPy；其他依赖由该环境管理。脚本会在本机以当前用户身份运行。修改代码或安装依赖后，点击「刷新节点」。

节点进入现有算子库，支持标签、ID、中英文关键词搜索、参数控件、连线与运行诊断。加载失败的包列出具体问题，其余有效节点继续可用。工作流只保存 ID、版本、参数；缺包时保留节点和连线，重新安装对应包并刷新即可恢复。

## 编写节点

```python
from lyflow_python import node


@node(
    id="py.example.offset",
    version="1.0.0",
    label="点云高度偏移",
    keywords=["高度", "偏移", "height", "offset"],
    inputs={"cloud": "PointCloud"},
    outputs={"cloud": "PointCloud"},
    params={"offset": {"type": "float", "default": 0.0, "unit": "m"}},
)
def offset(inputs, params, ctx):
    cloud = inputs["cloud"].copy()
    cloud.xyz[:, 2] += params["offset"]
    ctx.progress(1.0, "完成")
    return {"cloud": cloud}
```

单文件发现其中的装饰函数；包目录只加载从 `__init__.py` 导出的装饰函数。ID 使用 `py.<包名>.<节点名>`，在所有包间唯一。重复 ID 不注册并显示冲突。参数描述复用现有 manifest 参数类型。升级节点时明确修改 `version`；工作流版本迁移继续使用引擎已有规则。

Python 节点可以从 `2.0.0` 等主版本开始发布，新建节点保存当前版本。首版 SDK 不提供迁移回调；已有工作流与当前节点主版本不一致时，会明确报版本不匹配，需安装兼容版本或显式调整工作流。

SDK 随运行库放在 `python/lyflow_python`，worker 自动加入 SDK 搜索路径。独立编写或测试时，可安装 `python/` 下的 SDK，或将其加入 `PYTHONPATH`。示例见 `python/examples/offset.py` 和 `python/examples/image_metrics/`。

## 数据与上下文

| 端口 | Python 对象 | 约定 |
|---|---|---|
| PointCloud | `PointCloud(xyz, intensity=None, normals=None, rgb=None)` | xyz 为 float32 `[N,3]`，坐标单位米；可选 intensity 为 float32 `[N]`，normals 为 float32 `[N,3]`，rgb 为 uint8 `[N,3]`。 |
| Image | `Image(pixels)` | `[H,W]` 或 `[H,W,C]`，C 为 1、3、4；uint8、uint16 或 float32，RGB/RGBA 顺序。浮点图像不自动归一化。 |
| Measurement | `Measurement(value, unit=...)` | 引擎测量对象，携带有效性、单位等字段。有效数值必须有限。 |
| Record | `Record(data, record_type=...)` | JSON 值及记录类型。 |

输入数组只读。通过 `.copy()` 创建可写副本。返回字典的键必须与声明的输出端口完全一致；类型、维度、通道长度或数值错误成为当前节点诊断，不修改上游结果。数组通过请求专用临时文件与 NumPy memmap 交换，返回后复制到引擎自有数据并清理临时文件。不要把输入映射保存到下一次调用。

`ctx.progress(ratio, message)` 报告进度，`ctx.log(message, level="info")` 记录日志，`ctx.check_cancelled()` 检查取消。`ctx.base_dir` 是工作流目录，`ctx.thread_budget` 是线程预算；文件参数应相对工作流目录解析。print 与 stderr 也进入运行日志；异常完整 traceback 写入日志，诊断显示简要原因。

## 配置与运行

桌面和 CLI 共用 `%APPDATA%/com.lyflow.app/python-nodes.json`；`LYFLOW_PYTHON_CONFIG` 可指定独立配置文件：

```json
{
  "executable": "C:/work/venv/Scripts/python.exe",
  "packagePaths": ["C:/work/nodes/offset.py", "C:/work/nodes/image_metrics"],
  "timeoutMs": 60000
}
```

```powershell
lyflow python configure --config ./python-nodes.json
lyflow python settings
lyflow python refresh
lyflow run ./example.lyflow.json --outputs --summary
```

未配置解释器时不会启动 Python，原生工作流正常运行。每个配置环境使用一个常驻 worker，Python 请求串行执行，模块初始化结果可复用；原生节点仍按现有引擎机制调度。Python 节点关闭结果缓存，上游原生缓存保留。

加载或执行默认超时 60 秒。取消先发送协作信号，500 ms 后仍未结束则终止 worker 进程树。超时、崩溃或强制取消后，本次任务失败或取消，下次任务重新启动 worker，不自动重试失败任务。重建后若节点声明发生变化，会提示手动刷新，避免用旧端口和参数调用新定义。刷新在执行维护窗口内取消并等待正在运行的任务，再重新加载代码和依赖。单次 stdout/stderr 超过 1 MiB 时保留末尾并提示截断；异常 traceback 单独写入日志。

## 嵌入宿主

C ABI 新增可选导出 `lyflow_configure_python(config_json)`、`lyflow_python_status()`、`lyflow_refresh_python()`；返回 JSON 字符串，由 `lyflow_string_free` 释放。配置和刷新需在没有活动运行时调用；状态查询不启动 worker。C++ `Core` 的 `configurePython/pythonStatus/refreshPython` 和 Rust `Core` 的 `configure_python/python_status/refresh_python` 包装同一能力。旧运行库仍可加载和执行原生图，可先用 C++ `supportsPython()` 或 Rust `supports_python()` 探测支持。

Rust/Tauri `HostConfig.python: Some(PythonRuntimeConfig { ... })` 覆盖共享设置，并让界面只读；None 使用共享配置。三个 IPC 命令是 `get_python_settings`、`set_python_settings`、`refresh_python_nodes`。Transport 对应方法均为可选能力，旧宿主或静态传输显示不支持。

HTTP 接口见 [HTTP 传输契约](http-transport.md)。运行库 ZIP、SDK 与 NuGet 携带 SDK、worker 和示例；Python 解释器与第三方依赖由用户提供。

## 验证

```powershell
$env:PYTHONPATH = Join-Path (Get-Location) 'python'
python -B -m unittest discover -s python/tests -v
$env:LYFLOW_TEST_PYTHON = (Get-Command python).Source
ctest --test-dir build/core -C RelWithDebInfo --output-on-failure --no-tests=error
```

CI 使用 Python 3.12 和 NumPy 2.2.6，执行 SDK、真实 worker 与 C++ 接入测试。编辑器测试保留主线回归，按功能契约收敛到少于 100 项，并覆盖 Python 配置、只读宿主、失败状态和 Transport 接线。

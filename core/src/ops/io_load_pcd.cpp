#include "ops.h"

namespace lyflow::ops {

void registerIoLoadPcd(Registry& r) {
  OperatorDesc op;
  op.id = "io.load_pcd";
  op.version = "1.0.0";
  op.label = "Load PCD";
  op.category = "IO/Input";
  op.keywords = {"load", "read", "open", "pcd", "ply", "读取", "载入"};
  op.doc = "从磁盘读取点云文件。支持 PCD 与 PLY。";

  // 无输入端口：这是一个源节点。
  op.outputs = {
      Port{"cloud", "PointCloud", "Cloud", "读出的点云。", true},
  };

  Param path;
  path.name = "path";
  path.type = ParamType::Path;
  path.label = "File";
  path.doc = "点云文件路径。相对路径相对于当前图文件所在目录。";
  path.def = Value::text("");
  path.mode = "open";
  path.filters = {
      FileFilter{"Point Cloud", {"pcd", "ply"}},
      FileFilter{"All Files", {"*"}},
  };

  Param recenter;
  recenter.name = "recenter";
  recenter.type = ParamType::Bool;
  recenter.label = "Recenter To Origin";
  recenter.doc = "读入后把质心平移到原点。调试视角时方便，生产流程一般关掉。";
  recenter.def = Value::boolean(false);
  recenter.advanced = true;

  op.params = {path, recenter};

  // 读盘可以中断；没有降级预览的意义（要么读完要么没读）；
  // 同一路径同一文件内容 -> 同一结果，可缓存。
  op.capabilities = {/*cancellable=*/true, /*previewable=*/false, /*deterministic=*/true};

  r.addOperator(std::move(op));
}

}  // namespace lyflow::ops

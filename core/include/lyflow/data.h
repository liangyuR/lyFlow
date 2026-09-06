#pragma once
// 算子之间流动的数据。SoA 坐标、shared_ptr<const T> 零拷贝共享、
// 可选通道而非独立类型、点云带进程内唯一 id —— 四条理由见 core/README.md「数据模型」。
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lyflow/status.h"

namespace lyflow {

/// 轴对齐包围盒。空点云时 valid=false。
struct Bounds {
  float min[3] = {0, 0, 0};
  float max[3] = {0, 0, 0};
  bool valid = false;

  void extend(float x, float y, float z);
};

/// 无序点云。xyz 交错存放（x0,y0,z0,x1,y1,z1,...），其余通道逐点对齐。
struct PointCloud {
  /// 进程内唯一，构造时自增分配。Indices 靠它对账。
  std::uint64_t id;

  /// size() == 3 * pointCount()
  std::vector<float> xyz;
  /// 空 或 n
  std::vector<float> intensity;
  /// 空 或 3n
  std::vector<float> normals;
  /// 空 或 3n，逐分量 0..255
  std::vector<std::uint8_t> rgb;

  PointCloud();

  std::size_t pointCount() const { return xyz.size() / 3; }
  bool hasIntensity() const { return !intensity.empty(); }
  bool hasNormals() const { return !normals.empty(); }
  bool hasRgb() const { return !rgb.empty(); }

  void reserve(std::size_t n);
  void push(float x, float y, float z) {
    xyz.push_back(x);
    xyz.push_back(y);
    xyz.push_back(z);
  }

  Bounds bounds() const;

  /// 唯一的「按下标取子集」入口。滤波类算子必须经它出结果，
  /// 否则 intensity/normals/rgb 会被漏搬（core/README.md「写 compute 的约定」）。
  PointCloud select(const std::vector<std::int32_t>& keep) const;
  PointCloud selectInverse(const std::vector<std::int32_t>& drop) const;

  /// 通道齐不齐。执行器在算子返回后查一次，作者写错了立刻炸而不是流到下游。
  bool channelsConsistent() const;
};

/// 点下标集合。指向某个点云，本身不含坐标。
struct Indices {
  /// 来源点云的 id。消费方必须校验，不符是 bad_input 而不是越界崩溃。
  std::uint64_t sourceCloudId = 0;
  std::vector<std::int32_t> values;
};

/// 4x4 变换矩阵，行主序。
struct Transform {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

/// 平面：n·p + d = 0，n 已归一化。
struct Plane {
  float normal[3] = {0, 0, 1};
  float d = 0;
};

// 以下六种是 2D 量测域的通用载荷。坐标一律是**米**，与点云同单位；
// 只有 Measurement 的 value 例外，它带自己的 unit（通常是 mm）。

/// 轴对齐 2D 包围盒。
struct Box2D {
  float min[2] = {0, 0};
  float max[2] = {0, 0};
};

/// 2D 直线：过 point、方向 dir（单位向量）。可选带两个端点变成线段。
struct Line2D {
  float point[2] = {0, 0};
  float dir[2] = {1, 0};
  bool hasSegment = false;
  float start[2] = {0, 0};
  float end[2] = {0, 0};
};

struct Circle2D {
  float center[2] = {0, 0};
  float radius = 0;
};

struct Point2D {
  float p[2] = {0, 0};
};

/// 一次测量的结果。value 非有限表示没测出来，此时 ok=false 且 message 说明原因。
struct Measurement {
  double value = 0;
  bool ok = false;
  std::string unit = "mm";
  std::string message;
  /// 判定结果："" 未判定 / ok / high / low / margin
  std::string verdict;
  bool hasLimits = false;
  double nominal = 0;
  double upper = 0;
  double lower = 0;
};

/// 稠密 float32 张量，行主序。推理算子的输入输出（ADR-0015）。
/// 不进二进制 IPC —— Inspector 只看形状与 min/max/mean。
struct Tensor {
  std::vector<std::int64_t> shape;
  std::vector<float> data;

  /// shape 各维之积。shape 为空时是 0（不是标量 1）。
  std::size_t elementCount() const;
  /// data.size() 与 shape 对得上。
  bool consistent() const { return data.size() == elementCount(); }
  std::string shapeString() const;
};

/// 带类型标签的 JSON。算子包用它定义领域结构而不必改 core（ADR-0013）。
struct Record {
  std::string type;
  nlohmann::json data = nlohmann::json::object();
};

/// 端口上流动的值。类型标签必须和 manifest 的端口类型对得上。
class Data {
 public:
  enum class Kind {
    None, PointCloud, Indices, Transform, Plane,
    Box2D, Line2D, Circle2D, Point2D, Measurement, Record, Tensor, Error,
  };

  Data() = default;

  static Data cloud(std::shared_ptr<const PointCloud> c);
  static Data cloud(PointCloud c) { return cloud(std::make_shared<const PointCloud>(std::move(c))); }
  static Data indices(std::shared_ptr<const Indices> i);
  static Data indices(Indices i) { return indices(std::make_shared<const Indices>(std::move(i))); }
  static Data transform(std::shared_ptr<const Transform> t);
  static Data transform(Transform t) { return transform(std::make_shared<const Transform>(t)); }
  static Data plane(std::shared_ptr<const Plane> p);
  static Data plane(Plane p) { return plane(std::make_shared<const Plane>(p)); }
  static Data box2d(lyflow::Box2D b);
  static Data line2d(lyflow::Line2D l);
  static Data circle2d(lyflow::Circle2D c);
  static Data point2d(lyflow::Point2D p);
  static Data measurement(lyflow::Measurement m);
  static Data record(lyflow::Record r);
  static Data tensor(std::shared_ptr<const lyflow::Tensor> t);
  static Data tensor(lyflow::Tensor t) {
    return tensor(std::make_shared<const lyflow::Tensor>(std::move(t)));
  }
  static Data error(lyflow::Status s);

  Kind kind() const { return kind_; }
  bool empty() const { return kind_ == Kind::None; }

  /// 类型不符时返回 nullptr —— 调用方必须检查。执行器在调算子前已经按
  /// manifest 校验过端口类型，所以算子内部拿到 nullptr 属于内部错误。
  const PointCloud* asCloud() const;
  const Indices* asIndices() const;
  const Transform* asTransform() const;
  const Plane* asPlane() const;
  const lyflow::Box2D* asBox2D() const;
  const lyflow::Line2D* asLine2D() const;
  const lyflow::Circle2D* asCircle2D() const;
  const lyflow::Point2D* asPoint2D() const;
  const lyflow::Measurement* asMeasurement() const;
  const lyflow::Record* asRecord() const;
  const lyflow::Tensor* asTensor() const;
  const lyflow::Status* asError() const;
  bool isError() const { return kind_ == Kind::Error; }

  std::shared_ptr<const PointCloud> cloudPtr() const { return cloud_; }
  std::shared_ptr<const lyflow::Tensor> tensorPtr() const { return tensor_; }

  /// 对应 manifest 里的端口类型名，用于错误信息与事件里的 stats。
  const char* typeName() const;

  /// 粗略的内存占用，给事件里的统计用。所有通道都算进去。
  std::size_t byteSize() const;

  /// 点数 / 元素数。点云 = 点数，Indices = 下标个数，其余 = 1。
  std::size_t elementCount() const;

  /// 给 Inspector / 3D 叠画看的可读 JSON。点云与 Indices 返回空串（太大，走二进制）。
  std::string valueJson() const;

 private:
  Kind kind_ = Kind::None;
  std::shared_ptr<const PointCloud> cloud_;
  std::shared_ptr<const Indices> indices_;
  std::shared_ptr<const Transform> transform_;
  std::shared_ptr<const Plane> plane_;
  std::shared_ptr<const lyflow::Box2D> box2d_;
  std::shared_ptr<const lyflow::Line2D> line2d_;
  std::shared_ptr<const lyflow::Circle2D> circle2d_;
  std::shared_ptr<const lyflow::Point2D> point2d_;
  std::shared_ptr<const lyflow::Measurement> measurement_;
  std::shared_ptr<const lyflow::Record> record_;
  std::shared_ptr<const lyflow::Tensor> tensor_;
  std::shared_ptr<const lyflow::Status> error_;
};

/// manifest 端口类型名 -> Data::Kind。未知类型（含 "Any"）返回 None。
Data::Kind kindFromTypeName(const std::string& typeName);

/// Data::Kind -> manifest 端口类型名。
const char* typeNameFromKind(Data::Kind kind);

}  // namespace lyflow

#pragma once
// 算子之间流动的数据。SoA 坐标、shared_ptr<const T> 零拷贝共享、
// 可选通道而非独立类型、点云带进程内唯一 id —— 四条理由见 core/README.md「数据模型」。
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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

/// 端口上流动的值。类型标签必须和 manifest 的端口类型对得上。
class Data {
 public:
  enum class Kind { None, PointCloud, Indices, Transform, Plane };

  Data() = default;

  static Data cloud(std::shared_ptr<const PointCloud> c);
  static Data cloud(PointCloud c) { return cloud(std::make_shared<const PointCloud>(std::move(c))); }
  static Data indices(std::shared_ptr<const Indices> i);
  static Data indices(Indices i) { return indices(std::make_shared<const Indices>(std::move(i))); }
  static Data transform(std::shared_ptr<const Transform> t);
  static Data transform(Transform t) { return transform(std::make_shared<const Transform>(t)); }
  static Data plane(std::shared_ptr<const Plane> p);
  static Data plane(Plane p) { return plane(std::make_shared<const Plane>(p)); }

  Kind kind() const { return kind_; }
  bool empty() const { return kind_ == Kind::None; }

  /// 类型不符时返回 nullptr —— 调用方必须检查。执行器在调算子前已经按
  /// manifest 校验过端口类型，所以算子内部拿到 nullptr 属于内部错误。
  const PointCloud* asCloud() const;
  const Indices* asIndices() const;
  const Transform* asTransform() const;
  const Plane* asPlane() const;

  std::shared_ptr<const PointCloud> cloudPtr() const { return cloud_; }

  /// 对应 manifest 里的端口类型名，用于错误信息与事件里的 stats。
  const char* typeName() const;

  /// 粗略的内存占用，给事件里的统计用。所有通道都算进去。
  std::size_t byteSize() const;

  /// 点数 / 元素数。点云 = 点数，Indices = 下标个数，其余 = 1。
  std::size_t elementCount() const;

 private:
  Kind kind_ = Kind::None;
  std::shared_ptr<const PointCloud> cloud_;
  std::shared_ptr<const Indices> indices_;
  std::shared_ptr<const Transform> transform_;
  std::shared_ptr<const Plane> plane_;
};

/// manifest 端口类型名 -> Data::Kind。未知类型（含 "Any"）返回 None。
Data::Kind kindFromTypeName(const std::string& typeName);

/// Data::Kind -> manifest 端口类型名。
const char* typeNameFromKind(Data::Kind kind);

}  // namespace lyflow

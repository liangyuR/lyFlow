#pragma once
//
// 算子之间流动的数据。
//
// 四条设计决定：
//
// 1. **点云用 SoA**，坐标一块连续的 float 数组。降采样、裁剪这类算子是纯内存
//    带宽受限的，SoA 让它们能顺序扫；给前端做预览时也可以直接把坐标缓冲整块
//    丢过去，不用逐点重排（M2 的二进制 IPC 就是这么干的）。
//
// 2. **数据用 shared_ptr<const T> 传递**。一个输出端口连多个输入是常态，
//    共享只读引用意味着零拷贝；也为 M3 的跨运行缓存留好了路 —— 缓存命中时
//    直接把同一份 shared_ptr 交出去。const 是关键：算子拿到输入后不能就地改。
//
// 3. **intensity / normals / rgb 是可选通道，不是另一个类型**（D10）。
//    原来那个 PointCloudXYZI 端口类型已经删掉了：类型系统说一套、数据模型
//    做一套，迟早会在「XYZI 连到 XYZ 端口之后强度去哪了」这种问题上翻车。
//
// 4. **点云有进程内唯一 id**。Indices 只是一串下标，脱离它所指的点云毫无意义；
//    有了 id，extract_indices 才能在拿到张冠李戴的下标时报错，而不是默默
//    索引越界或者取出一堆无关的点。
//
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

  /// 唯一的「按下标取子集」入口。
  ///
  /// 所有滤波类算子必须经它出结果 —— 否则每个算子都要记得手工搬运 intensity /
  /// normals / rgb，而漏搬的表现是「下游的着色突然没了」，没人会想到是上游
  /// 某个滤波器的锅。集中在这里，加一个通道只改一处。
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

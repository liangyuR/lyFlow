#pragma once
// 图像像素叠画：平台级的 Record 类型 `lyflow.overlay2d`，
//   { frame: "image", items: [ { kind, role, …几何…, label? } ] }
// 坐标一律是**图像像素**（x 向右、y 向下、像素中心在整数坐标），接了标定、改出 mm 也照样对得上图。
// 格式由 schema/overlay2d.schema.json 定；包用这里的构造器产出，不手拼 JSON。
// 颜色、线宽由前端按 role 决定（未知 role 用默认色）—— C++ 给事实与语义，前端给呈现（ADR-0019）。
//
// 与主线带 Unit2D::Pixel 的几何类型并存；多组点、折线、文字等复合叠画使用此 Record。
#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lyflow/data.h"

namespace lyflow {

/// Record::type。前端按它认出叠画。
inline constexpr const char* kOverlay2DType = "lyflow.overlay2d";

/// 一个图像像素坐标 (x, y)。
using Px = std::array<double, 2>;

/// 叠画的构造器。每个方法追加一项并返回自己，可以链着写。
/// 非有限的坐标（NaN / Inf）进不了 JSON：点集里的那几个点被丢掉，几何本身不成立的那一项整个不加
/// （点集为空、折线不到两个点、圆心或半径非有限、半径为负）—— 叠画是给人看的，缺一笔好过整份坏掉。
class Overlay2D {
 public:
  /// 一组离散的点（画成圆点还是十字由前端按 role 定）。
  Overlay2D& points(const std::string& role, const std::vector<Px>& pts,
                    const std::string& label = {});
  /// 折线。closed = 首尾相连（多边形）。
  Overlay2D& polyline(const std::string& role, const std::vector<Px>& pts, bool closed = false,
                      const std::string& label = {});
  /// 一组互不相连的线段。JSON 里写成 points，两两一段（点数总是偶数）。
  Overlay2D& segments(const std::string& role, const std::vector<std::array<Px, 2>>& segs,
                      const std::string& label = {});
  Overlay2D& circle(const std::string& role, Px center, double radius,
                    const std::string& label = {});
  /// 轴对齐的框。两个角给反了也行，写出时按 min / max 排好。
  Overlay2D& box(const std::string& role, Px corner0, Px corner1, const std::string& label = {});
  /// 一段文字，at 是锚点：文字从这里向右写、竖直方向以它为基线。
  Overlay2D& text(const std::string& role, const std::string& text, Px at);

  /// 把另一份的全部项接在后面（判定积木把各步的结论画在一起时用）。
  Overlay2D& append(const Overlay2D& other);

  std::size_t size() const { return items_.size(); }
  bool empty() const { return items_.empty(); }

  /// `{ frame: "image", items: [...] }`，即 Record 的 data。
  nlohmann::json json() const;
  /// Record{ type: "lyflow.overlay2d", data: json() }。
  Record record() const;
  /// 直接写进端口：`outputs.set("overlay", overlay.data())`。
  Data data() const;

 private:
  nlohmann::json items_ = nlohmann::json::array();
};

}  // namespace lyflow

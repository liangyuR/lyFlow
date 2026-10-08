#pragma once
// 人造断胶（glue-plan §4 第 16 条的数据集）：在一帧满胶图上，把胶路一侧干净的背景平移过来盖住一段胶。
// 两侧都试，挑源带更干净的一侧（源带里响应 > contrastMin 的采样超过 dirtyMax 就不用它，两侧都不干净就不做），
// 羽化约 3 px；不用 inpaint —— 它会把胶色补回去。检测不在这里：出的图交给另一条检测链去查。
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "bead.h"

namespace lyflow::packs::glue {

struct SynthSpec {
  double sStart = 200;
  double length = 30;
  double margin = 4;        ///< 盖住的带子比胶两边各宽出这么多
  double shiftExtra = 12;   ///< 源带在盖住的带子外再错开这么多
  double dirtyMax = 0.02;
  double contrastMin = 16;
  double feather = 1.5;     ///< 羽化的高斯 σ（过渡约 3 px）
};

struct SynthResult {
  bool ok = false;
  std::string reason;
  int side = 0;             ///< 源带在哪一侧：+1 法向（行进方向右侧），−1 左侧
  double dirtyRight = 0;
  double dirtyLeft = 0;
  int stations = 0;         ///< 断口里原本有胶的站数
  double sFrom = 0;         ///< 盖住的那一段（原图胶路的 s），就是断口的真值
  double sTo = 0;
};

/// stations 是这一帧（满胶）量出来的站；path 是它的胶路。成功时 out 是改过的单通道图，否则 out 是原图的拷贝。
SynthResult synthBreak(const cv::Mat& gray, const PolylineView& path,
                       const std::vector<Station>& stations, double stationStep, bool bright,
                       int widthMax, const SynthSpec& spec, cv::Mat& out);

}  // namespace lyflow::packs::glue

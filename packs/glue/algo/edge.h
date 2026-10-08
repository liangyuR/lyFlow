#pragma once
// 量胶边距（glue-plan D8 / D9）：胶靠零件边那一侧的胶边 → 零件边，沿胶路法向量。
// 零件边在哪一侧自动判（两侧都找，取多数站找到强边的一侧），不设人要填的 side。
#include <vector>

#include <opencv2/core.hpp>

#include "bead.h"

namespace lyflow::packs::glue {

struct EdgeSpec {
  double searchLength = 200;  ///< 从胶边外 3 px 起沿法向找多远
  double contrastMin = 40;    ///< 跳变两侧的灰度差至少这么大
  double farRun = 12;         ///< 远侧要持续这么长都是暗的（翻边上的划痕、反光条挡在这里）
  bool darkToBright = false;  ///< edgePolarity：默认从胶往外是 亮 → 暗
  bool fromCenter = false;    ///< reference = bead_center：从胶的中线量，默认从近边量
};

struct EdgeStation {
  bool searched = false;  ///< 这一站有胶，两侧都找过
  bool found = false;     ///< 选中的那一侧找到了边
  bool kept = false;      ///< 找到了、也没被中值 + MAD 剔掉
  double edgeOffset = 0;  ///< 零件边沿 side·n 相对胶路点的偏移（px）
  double refOffset = 0;   ///< 量起点（近边或中线）沿 side·n 的偏移
  double distancePx = 0;
  P2 edge;                ///< 零件边点
  P2 from;                ///< 量起点
};

struct EdgeResult {
  int side = 0;  ///< +1 = 法向那一侧（行进方向右侧），−1 = 左侧，0 = 两侧都没找到
  int votesRight = 0;
  int votesLeft = 0;
  std::vector<EdgeStation> stations;  ///< 与输入的站一一对应
};

EdgeResult measureEdges(const cv::Mat& gray, const std::vector<Station>& stations,
                        const EdgeSpec& spec);

/// 沿 c + dir·t（t 从 start 起，1 px 一步）找第一个合格的跳变（D9），返回它的 t（亚像素）；找不到返回 NaN。
/// 合格 = 极性对、两侧差 ≥ contrastMin、远侧 farRun px 都比近侧暗至少一半的 contrastMin。位置取跳变里
/// 灰度下降最快的那一点（三点抛物线细化）。图外的采样截断剖面，不当成暗。
double findPartEdge(const Field& gray, P2 c, P2 dir, double start, const EdgeSpec& spec);

}  // namespace lyflow::packs::glue

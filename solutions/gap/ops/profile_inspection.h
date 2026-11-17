#pragma once
#include "lyflow/operator.h"
#include "lyflow/registry.h"

namespace lyflow::packs::gap::profile_scene {
struct CornerInputs {
  Data lineLeft, lineLeftContext, lineRight, lineRightContext;
  Data baseLine, baseLineContext, origin, originContext;
};
struct CornerResult {
  Data vertex, vertexContext, gap, flush, angle, segment, segmentContext;
  Record quality;
};
struct JointInputs {
  Data primary, secondary, primaryProvenance, secondaryProvenance;
};
struct JointResult {
  Data gap, flush, baseLine, refLine, gapSegment, flushSegment, feature;
  Data baseLineContext, refLineContext, gapSegmentContext, flushSegmentContext, pointContext;
  Record quality, qualityBase, qualityRef;
};
std::vector<Param> cornerInspectionParams();
std::vector<Param> grooveInspectionParams();
std::vector<Param> notchInspectionParams();
Status measureProfileCorner(const CornerInputs&, const ParamView&, CornerResult&, ExecContext&);
Status measureProfileGroove(const JointInputs&, const ParamView&, JointResult&, ExecContext&);
Status measureProfileNotch(const JointInputs&, const ParamView&, JointResult&, ExecContext&);
void registerProfileInspection(Registry&);
namespace inspection_detail {
Status cornerGeometry(const Inputs&, const ParamView&, Outputs&, ExecContext&, const Point2D*);
Status grooveClouds(const Inputs&, const ParamView&, Outputs&, ExecContext&);
Status notchClouds(const Inputs&, const ParamView&, Outputs&, ExecContext&);
}
}

#include "lyflow/cloud_io.h"

#include <atomic>

namespace lyflow {
namespace {

std::atomic<CloudWriterFn>& writer() {
  static std::atomic<CloudWriterFn> fn{nullptr};
  return fn;
}

}  // namespace

void setCloudWriter(CloudWriterFn fn) { writer().store(fn); }

Status saveCloudToFile(const PointCloud& cloud, const std::filesystem::path& file,
                       const std::string& format) {
  const CloudWriterFn fn = writer().load();
  if (!fn) {
    return Status::Error(Phase::Execute, "unsupported",
                         "本次构建没有点云写盘算子包（见 docs/op-packs.md）");
  }
  return fn(cloud, file, format);
}

}  // namespace lyflow

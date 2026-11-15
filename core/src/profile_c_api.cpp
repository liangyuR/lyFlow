#include "lyflow/c_api.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>

#include "exec/result_store.h"
#include "lyflow/data.h"
#include "lyflow/profile.h"
#include "profile_input.h"

namespace {
char* lyflow_profile_input_message(const char* text) {
  const auto size = std::strlen(text) + 1;
  auto* copy = static_cast<char*>(std::malloc(size));
  if (copy)
    std::memcpy(copy, text, size);
  return copy;
}
struct ProfileHandle {
  lyflow::Data data;
  std::string descriptor;
};
}

extern "C" {
char* lyflow_profile_input_create(const lyflow_run_profile_input* input,
                                  lyflow_profile_input** out) {
  if (out)
    *out = nullptr;
  try {
    if (!input || !out)
      throw std::invalid_argument("Profile input/output is null");
    auto data = lyflow::runtime::copyProfileInput(*input);
    if (const auto* error = data.asError())
      throw std::invalid_argument(error->message);
    auto handle = std::make_unique<lyflow_profile_input>();
    handle->data = std::move(data);
    *out = handle.release();
    return lyflow_profile_input_message("");
  } catch (const std::exception& e) {
    return lyflow_profile_input_message(e.what());
  } catch (...) {
    return nullptr;
  }
}

char* lyflow_profile_input_capture(const char* run_id, const char* node_id, const char* port,
                                   lyflow_profile_input** out) {
  if (out)
    *out = nullptr;
  try {
    if (!out)
      throw std::invalid_argument("Profile output is null");
    auto handle = std::make_unique<lyflow_profile_input>();
    if (!lyflow::exec::ResultStore::instance().get(run_id ? run_id : "", node_id ? node_id : "",
                                                   port ? port : "", handle->data) ||
        !handle->data.asProfile())
      throw std::invalid_argument("Profile output is unavailable");
    *out = handle.release();
    return lyflow_profile_input_message("");
  } catch (const std::exception& e) {
    return lyflow_profile_input_message(e.what());
  } catch (...) {
    return nullptr;
  }
}

void lyflow_profile_input_free(lyflow_profile_input* input) { delete input; }

int lyflow_output_profile(const char* run_id, const char* node_id, const char* port,
                          uint32_t offset, uint32_t count, lyflow_profile_view* out) {
  if (!out)
    return 2;
  std::memset(out, 0, sizeof(*out));
  try {
    auto handle = std::make_unique<ProfileHandle>();
    if (!lyflow::exec::ResultStore::instance().get(run_id ? run_id : "", node_id ? node_id : "",
                                                   port ? port : "", handle->data))
      return 1;
    const auto* profile = handle->data.asProfile();
    if (!profile || !profile->consistent())
      return 1;
    const auto& cloud = *profile->cloud;
    const auto total = static_cast<uint32_t>(cloud.pointCount());
    const auto begin = std::min(offset, total);
    const auto size = count ? std::min(count, total - begin) : total - begin;
    handle->descriptor = lyflow::profileDescriptorJson(profile->descriptor).dump();
    out->point_count = size;
    out->total_points = total;
    out->offset = begin;
    out->flags = (!cloud.intensity.empty() ? LYFLOW_CLOUD_HAS_INTENSITY : 0u) |
                 (!cloud.normals.empty() ? LYFLOW_CLOUD_HAS_NORMALS : 0u) |
                 (!cloud.rgb.empty() ? LYFLOW_CLOUD_HAS_RGB : 0u);
    out->cloud_id = cloud.id;
    out->descriptor_json = handle->descriptor.c_str();
    if (size) {
      out->xyz = cloud.xyz.data() + 3 * static_cast<std::size_t>(begin);
      out->intensity = cloud.intensity.empty() ? nullptr : cloud.intensity.data() + begin;
      out->normals = cloud.normals.empty()
                         ? nullptr
                         : cloud.normals.data() + 3 * static_cast<std::size_t>(begin);
      out->rgb =
          cloud.rgb.empty() ? nullptr : cloud.rgb.data() + 3 * static_cast<std::size_t>(begin);
      out->validity = profile->validity.data() + begin;
      out->source_indices = profile->sourceIndices.data() + begin;
    }
    out->handle = handle.release();
    return 0;
  } catch (...) {
    return 3;
  }
}

void lyflow_profile_view_free(lyflow_profile_view* view) {
  if (!view)
    return;
  delete static_cast<ProfileHandle*>(view->handle);
  std::memset(view, 0, sizeof(*view));
}
}

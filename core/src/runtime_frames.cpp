#include "runtime_frames.h"
#include "lyflow/profile.h"

#include "exec/result_store.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <cstdio>
#include <fcntl.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

namespace lyflow::runtime {
using Json = nlohmann::json;
constexpr std::uint64_t MiB = 1024ull * 1024;
constexpr std::uint64_t maxFrameBytes = 2048 * MiB;
constexpr std::size_t maxEntries = 8192;

struct Failure : std::runtime_error {
  std::string code;
  Failure(std::string c, std::string text)
      : std::runtime_error(std::move(text)), code(std::move(c)) {}
};
void require(bool condition, const std::string& message, const char* code = "bad_input") {
  if (!condition)
    throw Failure(code, message);
}
struct Cancellation {
  lyflow_cancel_cb callback = nullptr;
  void* user = nullptr;
  void check() const { require(!callback || callback(user) == 0, "操作已取消", "cancelled"); }
};
thread_local Cancellation cancellation;
struct CancelScope {
  Cancellation previous = cancellation;
  CancelScope(lyflow_cancel_cb cb, void* user) { cancellation = {cb, user}; }
  ~CancelScope() { cancellation = previous; }
};
Json parse(const char* text, const Json& fallback = Json::object()) {
  if (!text || !*text)
    return fallback;
  require(std::strlen(text) <= MiB, "JSON超过1MiB预算", "resource_limit");
  return Json::parse(text);
}
std::uint64_t decimal(const Json& value, const char* name) {
  require(value.is_string(), std::string(name) + "必须为十进制u64字符串");
  const auto& s = value.get_ref<const std::string&>();
  std::uint64_t result = 0;
  const auto converted = std::from_chars(s.data(), s.data() + s.size(), result);
  require(!s.empty() && converted.ec == std::errc{} && converted.ptr == s.data() + s.size(),
          std::string(name) + "不是合法u64");
  return result;
}
std::string text(const Json& object, const char* name, std::size_t limit = 256) {
  require(object.contains(name) && object[name].is_string(), std::string(name) + "必须为字符串");
  auto value = object[name].get<std::string>();
  require(value.size() <= limit && value.find('\0') == std::string::npos,
          std::string(name) + "长度或内容无效");
  return value;
}
void boundedJson(const Json& value, std::size_t depth = 0) {
  require(depth <= 16, "JSON嵌套超过16层", "resource_limit");
  require(!value.is_binary() && !value.is_discarded(), "JSON不得含隐式二进制");
  require(!value.is_number_float() || std::isfinite(value.get<double>()), "JSON数值必须有限");
  if (value.is_array() || value.is_object()) {
    require(value.size() <= 8192, "JSON集合超过8192项", "resource_limit");
    for (const auto& child : value)
      boundedJson(child, depth + 1);
  }
}
std::string unique(const char* prefix) {
  static std::atomic<std::uint64_t> counter{0};
  return std::string(prefix) +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
         std::to_string(counter.fetch_add(1));
}
struct Budget {
  std::mutex mutex;
  std::uint64_t bytes = 0;
  std::size_t payloads = 0;
};
std::shared_ptr<Budget> budget() {
  static auto value = std::make_shared<Budget>();
  return value;
}
struct Payload {
  std::vector<exec::CapturedOutput> entries;
  std::vector<exec::NamedOutput> named;
  std::uint64_t bytes = 0;
  std::shared_ptr<Budget> owner;
  ~Payload() {
    if (owner) {
      std::lock_guard<std::mutex> lock(owner->mutex);
      owner->bytes -= bytes;
      --owner->payloads;
    }
  }
  void account(std::uint64_t maximum) {
    std::uint64_t required = 0;
    require(entries.size() <= maxEntries && named.size() <= 4096, "帧端口数量超限",
            "resource_limit");
    for (const auto& entry : entries) {
      require(!entry.nodeId.empty() && !entry.port.empty() && !entry.data.empty(),
              "帧端口必须有名称和数据");
      if (const auto* profile = entry.data.asProfile()) {
        std::string why;
        const bool valid = validateProfile(*profile, why);
        require(valid, why);
      }
      const auto n = entry.data.byteSize();
      require(n <= maximum && required <= maximum - n, "帧载荷超过预算", "resource_limit");
      required += n;
    }
    auto state = owner ? owner : budget();
    std::lock_guard<std::mutex> lock(state->mutex);
    require((owner || state->payloads < 1024) &&
                state->bytes - (owner ? bytes : 0) <= 8192 * MiB - required,
            "进程帧所有权预算已满", "resource_limit");
    state->bytes = state->bytes - (owner ? bytes : 0) + required;
    if (!owner)
      ++state->payloads;
    bytes = required;
    owner = std::move(state);
  }
  void reserve(std::uint64_t count) {
    auto state = budget();
    std::lock_guard<std::mutex> lock(state->mutex);
    require(count <= maxFrameBytes && state->payloads < 1024 && state->bytes <= 8192 * MiB - count,
            "进程帧所有权预算已满", "resource_limit");
    state->bytes += count;
    ++state->payloads;
    bytes = count;
    owner = std::move(state);
  }
};
struct Frame {
  std::shared_ptr<Payload> payload;
  Json metadata;
  std::string view;
  Frame(std::shared_ptr<Payload> data, Json context)
      : payload(std::move(data)), metadata(std::move(context)), view(unique("frame-view-")) {
    exec::ResultStore::instance().addView(view, payload->entries, payload->named);
  }
  ~Frame() { exec::ResultStore::instance().removeView(view); }
  Json info() const {
    auto result = metadata;
    result["type"] = "lyflow.Frame.v1";
    result["schemaVersion"] = 1;
    result["viewRunId"] = view;
    result["payloadBytes"] = payload->bytes;
    result["entries"] = Json::parse(exec::ResultStore::instance().bindingsJson(view))["entries"];
    result["namedOutputs"] = Json::array();
    for (const auto& name : payload->named)
      result["namedOutputs"].push_back(
          {{"name", name.name}, {"nodeId", name.nodeId}, {"port", name.port}});
    return result;
  }
};
Json metadata(Json value, bool imported = false) {
  require(value.is_object(), "帧元信息必须为对象");
  boundedJson(value);
  require(value.dump().size() <= 256 * 1024, "帧元信息超过256KiB", "resource_limit");
  require(!text(value, "frameId").empty(), "frameId不得为空");
  decimal(value.at("timestampNs"), "timestampNs");
  if (!value.contains("frame"))
    value["frame"] = "";
  text(value, "frame");
  if (!value.contains("calibrationVersion"))
    value["calibrationVersion"] = nullptr;
  require(value["calibrationVersion"].is_null() || value["calibrationVersion"].is_string(),
          "calibrationVersion必须为字符串或null");
  if (value["calibrationVersion"].is_string())
    text(value, "calibrationVersion");
  if (value.contains("context")) {
    require(value["context"].is_object(), "context必须为对象");
    require(value["context"].dump().size() <= 64 * 1024, "context超过64KiB预算", "resource_limit");
  }
  if (value.contains("schemaVersion"))
    require(value["schemaVersion"] == 1, "帧元信息版本不支持", "unsupported_version");
  if (value.contains("type"))
    require(value["type"] == "lyflow.Frame.v1", "帧元信息类型不支持");
  if (!imported)
    require(!value.contains("session"), "会话上下文只能由会话分配");
  else if (value.contains("session")) {
    require(value["session"].is_object(), "session必须为对象");
    require(!text(value["session"], "id").empty() && !text(value["session"], "epoch").empty(),
            "会话身份不得为空");
    decimal(value["session"].at("sequence"), "sequence");
  }
  value.erase("viewRunId");
  value.erase("entries");
  value.erase("namedOutputs");
  value.erase("payloadBytes");
  return value;
}
std::uint64_t maximum(const Json& options) {
  require(options.is_object(), "options必须为对象");
  const auto n = options.value("maxBytes", std::uint64_t{1024 * MiB});
  require(n > 0 && n <= maxFrameBytes, "maxBytes必须在1..2GiB之间", "resource_limit");
  return n;
}
std::shared_ptr<Frame> capture(const std::string& run, Json context, const Json& options) {
  auto data = std::make_shared<Payload>();
  std::string summary, error;
  const bool complete =
      exec::ResultStore::instance().capture(run, data->entries, data->named, summary, error);
  require(complete, error, "missing_results");
  require(!summary.empty(), "仅可录制已完成的实际运行", "run_not_completed");
  const auto result = Json::parse(summary);
  require(result.value("status", "") == "ok", "失败或取消运行不可录为成功帧", "run_not_successful");
  context = metadata(std::move(context));
  context["sourceRunId"] = run;
  context["injectedInputs"] = Json::array();
  std::set<std::string> occupied;
  for (const auto& entry : data->entries)
    occupied.insert(entry.nodeId);
  std::map<std::string, std::string> inputNodes;
  for (const auto& entry : exec::ResultStore::instance().runInputs(run)) {
    auto& source = inputNodes[entry.nodeId];
    if (source.empty()) {
      source = "__frame_input__/" + entry.nodeId;
      while (occupied.count(source))
        source += "_";
      occupied.insert(source);
    }
    data->entries.push_back({source, entry.port, entry.data});
    context["injectedInputs"].push_back({{"sourceNode", source},
                                         {"sourcePort", entry.port},
                                         {"nodeId", entry.nodeId},
                                         {"port", entry.port}});
  }
  context["missing"] = Json::parse(exec::ResultStore::instance().bindingsJson(run))["missing"];

  if (options.contains("sourceNodes")) {
    require(options["sourceNodes"].is_array() && options["sourceNodes"].size() <= 4096,
            "sourceNodes必须为有界列表");
    for (const auto& node : options["sourceNodes"])
      require(node.is_string() && !node.get_ref<const std::string&>().empty(),
              "sourceNodes项必须是节点名");
    context["context"]["sourceNodes"] = options["sourceNodes"];
  }
  if (options.contains("outputs")) {
    require(options["outputs"].is_array() && !options["outputs"].empty(),
            "outputs必须为非空名称列表");
    std::set<std::string> selected;
    for (const auto& name : options["outputs"])
      require(name.is_string() && selected.insert(name.get<std::string>()).second,
              "outputs不得重复");
    std::vector<exec::NamedOutput> names;
    for (const auto& name : data->named)
      if (selected.erase(name.name))
        names.push_back(name);
    require(selected.empty(), "outputs包含不存在的命名输出");
    data->named = std::move(names);
  }
  for (const auto& name : data->named) {
    Data output;
    require(exec::ResultStore::instance().get(run, name.nodeId, name.port, output),
            "命名输出未绑定实际数据", "missing_results");
  }
  data->account(maximum(options));
  context = metadata(std::move(context), true);
  return std::make_shared<Frame>(std::move(data), std::move(context));
}

struct Chunk {
  const std::uint8_t* data;
  std::size_t size;
  std::shared_ptr<std::vector<std::uint8_t>> owner;
};
bool sameBytes(const void* a, const void* b, std::size_t size) {
  return size == 0 || std::memcmp(a, b, size) == 0;
}
template <class T> bool same(const std::vector<T>& a, const std::vector<T>& b) {
  return a.size() == b.size() && sameBytes(a.data(), b.data(), a.size() * sizeof(T));
}
struct Encoder {
  std::vector<Chunk> chunks;
  std::map<std::uint64_t, const PointCloud*> clouds;
  std::uint64_t bytes = 0;
  Json blob(const void* data, std::size_t count,
            std::shared_ptr<std::vector<std::uint8_t>> owner = {}) {
    require(count <= maxFrameBytes && bytes <= maxFrameBytes - count, "归档载荷超过2GiB",
            "resource_limit");
    Json range{{"offset", bytes}, {"bytes", count}};
    chunks.push_back({static_cast<const std::uint8_t*>(data), count, std::move(owner)});
    bytes += count;
    return range;
  }
  template <class T> Json numbers(const T* values, std::size_t n) {
    auto owner = std::make_shared<std::vector<std::uint8_t>>(n * sizeof(T));
    if (n)
      std::memcpy(owner->data(), values, owner->size());
    const auto* pointer = owner->data();
    const auto size = owner->size();
    return blob(pointer, size, std::move(owner));
  }
  Json encode(const Data& data, std::size_t depth = 0) {
    require(depth <= 8, "Bundle嵌套超过预算");
    Json out{{"kind", data.typeName()}};
    if (const auto* c = data.asCloud()) {
      require(c->channelsConsistent(), "点云通道长度不一致");
      const auto key = std::to_string(c->id);
      out["cloudKey"] = key;
      const auto old = clouds.find(c->id);
      if (old != clouds.end()) {
        const auto* a = old->second;
        require(same(a->xyz, c->xyz) && same(a->intensity, c->intensity) &&
                    same(a->normals, c->normals) && same(a->rgb, c->rgb),
                "相同点云身份对应不同数据", "identity_conflict");
        out["alias"] = true;
      } else {
        clouds[c->id] = c;
        out["count"] = c->pointCount();
        out["xyz"] = blob(c->xyz.data(), c->xyz.size() * sizeof(float));
        out["intensity"] = blob(c->intensity.data(), c->intensity.size() * sizeof(float));
        out["normals"] = blob(c->normals.data(), c->normals.size() * sizeof(float));
        out["rgb"] = blob(c->rgb.data(), c->rgb.size());
      }
    } else if (const auto* profile = data.asProfile()) {
      std::string why;
      const bool valid = validateProfile(*profile, why);
      require(valid, why);
      out["descriptor"] = profileDescriptorJson(profile->descriptor);
      out["cloud"] = encode(Data::cloud(profile->cloud), depth + 1);
      out["validity"] = blob(profile->validity.data(), profile->validity.size());
      out["sourceIndices"] = blob(profile->sourceIndices.data(),
                                  profile->sourceIndices.size() * sizeof(std::uint32_t));
    } else if (const auto* image = data.asImage()) {
      require(image->consistent() && image->scale > 0, "图像不完整或scale无效");
      out["width"] = image->width;
      out["height"] = image->height;
      out["channels"] = image->channels;
      out["depth"] = static_cast<int>(image->depth);
      out["scale"] = image->scale;
      out["pixels"] = blob(image->pixels.get(), image->byteSize());
    } else if (const auto* tensor = data.asTensor()) {
      require(tensor->consistent() && tensor->shape.size() <= 32 && data.tensorScale() > 0,
              "Tensor形状或scale无效");
      out["shape"] = tensor->shape;
      out["scale"] = data.tensorScale();
      out["values"] = blob(tensor->data.data(), tensor->data.size() * sizeof(float));
    } else if (const auto* indices = data.asIndices()) {
      out["cloudKey"] = std::to_string(indices->sourceCloudId);
      out["values"] = blob(indices->values.data(), indices->values.size() * sizeof(std::int32_t));
    } else if (const auto* r = data.asRecord()) {
      boundedJson(r->data);
      require(r->type.size() <= 256 && r->type.find('\0') == std::string::npos &&
                  r->data.dump().size() <= MiB,
              "Record超过预算或类型无效");
      out["recordType"] = r->type;
      out["data"] = r->data;
    } else if (const auto* t = data.asTransform())
      out["values"] = numbers(t->m, 16);
    else if (const auto* p = data.asPlane()) {
      const float values[]{p->normal[0], p->normal[1], p->normal[2], p->d};
      out["values"] = numbers(values, 4);
    } else if (const auto* p = data.asPoint2D()) {
      out["unit"] = static_cast<int>(p->unit);
      out["values"] = numbers(p->p, 2);
    } else if (const auto* p = data.asCircle2D()) {
      out["unit"] = static_cast<int>(p->unit);
      const float values[]{p->center[0], p->center[1], p->radius};
      out["values"] = numbers(values, 3);
    } else if (const auto* p = data.asBox2D()) {
      out["unit"] = static_cast<int>(p->unit);
      const float values[]{p->min[0], p->min[1], p->max[0], p->max[1]};
      out["values"] = numbers(values, 4);
    } else if (const auto* p = data.asLine2D()) {
      out["unit"] = static_cast<int>(p->unit);
      out["hasSegment"] = p->hasSegment;
      const float values[]{p->point[0], p->point[1], p->dir[0], p->dir[1],
                           p->start[0], p->start[1], p->end[0], p->end[1]};
      out["values"] = numbers(values, 8);
    } else if (const auto* m = data.asMeasurement()) {
      const double values[]{m->value, m->nominal, m->upper, m->lower};
      out["values"] = numbers(values, 4);
      out["ok"] = m->ok;
      out["unit"] = m->unit;
      out["message"] = m->message;
      out["verdict"] = m->verdict;
      out["hasLimits"] = m->hasLimits;
    } else if (const auto* b = data.asBundle()) {
      require(b->fields.size() <= 4096 && !b->kind.empty(), "Bundle结构无效");
      out["bundleKind"] = b->kind;
      out["fields"] = Json::array();
      std::set<std::string> names;
      for (const auto& field : b->fields) {
        require(!field.first.empty() && names.insert(field.first).second, "Bundle字段不得重名");
        out["fields"].push_back(
            {{"name", field.first}, {"value", encode(field.second, depth + 1)}});
      }
    } else if (const auto* status = data.asError())
      out["status"] = {{"ok", status->ok},
                       {"phase", static_cast<int>(status->phase)},
                       {"code", status->code},
                       {"message", status->message},
                       {"paramPath", status->paramPath},
                       {"portName", status->portName}};
    else
      throw Failure("unsupported_type", "空Data不可归档");
    return out;
  }
};

const std::set<std::string>& referenceTypes() {
  static const std::set<std::string> types{"cloud.Groups.v1",
                                           "cloud.Statistics.v1",
                                           "cloud.GroupStatistics.v1",
                                           "cloud.TopLayers.v1",
                                           "cloud.NeighborHeights.v1",
                                           "cloud.EmptyTote.v1",
                                           "cloud.Template3D.v1",
                                           "cloud.PlanePatches.v1",
                                           "cloud.Edges3D.v1",
                                           "cloud.ViewSampling.v1",
                                           "cloud.ImageROI.v1",
                                           "cloud.PrimitiveCrop.v1",
                                           "cloud.MaskPrimitiveRemoval.v1",
                                           "cloud.HoleFill.v1",
                                           "inspection.Defects.v1",
                                           "inspection.Profiles.v1",
                                           "vision.ProjectionQuality.v1",
                                           "vision.DepthMapping.v1",
                                           "vision.CloudROIQuality.v1",
                                           "vision.DepthFilterQuality.v1"};
  return types;
}

struct Decoder {
  std::ifstream& file;
  std::uint64_t base, bytes, consumed = 0;
  std::map<std::string, Data> clouds;
  std::map<std::string, std::size_t> pointCounts;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
  template <class T> std::vector<T> read(const Json& range) {
    cancellation.check();
    require(range.is_object() && range.contains("offset") && range.contains("bytes") &&
                range["offset"].is_number_unsigned() && range["bytes"].is_number_unsigned(),
            "载荷范围无效");
    const auto offset = range["offset"].get<std::uint64_t>(),
               size = range["bytes"].get<std::uint64_t>();
    require(offset <= bytes && size <= bytes - offset && size % sizeof(T) == 0,
            "载荷长度/边界无效");
    require(consumed <= maxFrameBytes - size, "载荷累计分配超过预算", "resource_limit");
    if (size)
      ranges.emplace_back(offset, offset + size);
    consumed += size;
    std::vector<T> result(static_cast<std::size_t>(size / sizeof(T)));
    file.seekg(static_cast<std::streamoff>(base + offset));
    for (std::uint64_t position = 0; position < size;) {
      cancellation.check();
      const auto count = std::min<std::uint64_t>(MiB, size - position);
      file.read(reinterpret_cast<char*>(result.data()) + position,
                static_cast<std::streamsize>(count));
      position += count;
    }
    require(file.good(), "载荷被截断或读取失败", "io");
    return result;
  }
  void scanClouds(const Json& value, std::size_t depth = 0) {
    require(depth <= 8, "Bundle嵌套超限");
    const auto kind = text(value, "kind");
    if (kind == "PointCloud" && !value.value("alias", false)) {
      const auto key = text(value, "cloudKey");
      require(!clouds.count(key), "点云键重复");
      PointCloud cloud;
      cloud.xyz = read<float>(value.at("xyz"));
      cloud.intensity = read<float>(value.at("intensity"));
      cloud.normals = read<float>(value.at("normals"));
      cloud.rgb = read<std::uint8_t>(value.at("rgb"));
      require(cloud.channelsConsistent() && value.at("count") == cloud.pointCount(),
              "点云通道/点数不一致");
      pointCounts[key] = cloud.pointCount();
      clouds.emplace(key, Data::cloud(std::move(cloud)));
    } else if (kind == "Profile") {
      scanClouds(value.at("cloud"), depth + 1);
    } else if (kind.rfind("Bundle<", 0) == 0) {
      require(value.at("fields").is_array() && value.at("fields").size() <= 4096,
              "Bundle字段数无效");
      for (const auto& field : value.at("fields"))
        scanClouds(field.at("value"), depth + 1);
    }
  }
  Data decode(const Json& value, std::size_t depth = 0) {
    require(depth <= 8, "Bundle嵌套超限");
    const auto kind = text(value, "kind");
    if (kind == "PointCloud") {
      const auto key = text(value, "cloudKey");
      require(clouds.count(key), "点云别名未绑定");
      return clouds.at(key);
    }
    if (kind == "Profile") {
      Profile profile;
      std::string why;
      const bool descriptorValid =
          parseProfileDescriptor(value.at("descriptor"), profile.descriptor, why);
      require(descriptorValid, why);
      auto cloud = decode(value.at("cloud"), depth + 1);
      require(cloud.asCloud() != nullptr, "Profile归档必须包含PointCloud缓冲");
      profile.cloud = cloud.cloudPtr();
      profile.validity = read<std::uint8_t>(value.at("validity"));
      profile.sourceIndices = read<std::uint32_t>(value.at("sourceIndices"));
      const bool valid = validateProfile(profile, why);
      require(valid, why);
      return Data::profile(std::move(profile));
    }
    if (kind == "Image") {
      for (const char* field : {"width", "height", "channels", "depth", "scale"})
        require(value.at(field).is_number_integer() && value.at(field).get<std::int64_t>() > 0 &&
                    value.at(field).get<std::uint64_t>() <= std::numeric_limits<int>::max(),
                "图像维度必须为正int32整数");
      const int w = value.at("width"), h = value.at("height"), c = value.at("channels"),
                d = value.at("depth"), scale = value.at("scale");
      require(w > 0 && h > 0 && w <= (1 << 20) && h <= (1 << 20) && (c == 1 || c == 3 || c == 4) &&
                  (d == 1 || d == 2 || d == 4) && scale > 0,
              "图像尺寸/位深/scale无效");
      const std::uint64_t size = std::uint64_t(w) * h * c * d;
      require(size <= bytes && value.at("pixels").at("bytes") == size, "图像载荷尺寸不符");
      auto pixels = read<std::uint8_t>(value.at("pixels"));
      auto owner = std::make_shared<const std::vector<std::uint8_t>>(std::move(pixels));
      Image image;
      image.width = w;
      image.height = h;
      image.channels = c;
      image.depth = static_cast<PixelDepth>(d);
      image.pixels = std::shared_ptr<const std::uint8_t>(owner, owner->data());
      image.scale = scale;
      return Data::image(std::move(image));
    }
    if (kind == "Tensor") {
      Tensor tensor;
      require(value.at("shape").is_array() && value.at("shape").size() <= 32, "Tensor秩超过预算");
      for (const auto& axis : value.at("shape"))
        require(axis.is_number_integer() && axis.get<std::int64_t>() >= 0 &&
                    axis.get<std::uint64_t>() <=
                        std::uint64_t(std::numeric_limits<std::int64_t>::max()),
                "Tensor维度必须为非负int64整数");
      require(value.at("scale").is_number_integer() && value.at("scale").get<std::int64_t>() > 0 &&
                  value.at("scale").get<std::uint64_t>() <= std::numeric_limits<int>::max(),
              "Tensor scale必须为正int32整数");
      tensor.shape = value.at("shape").get<std::vector<std::int64_t>>();
      tensor.data = read<float>(value.at("values"));
      const int scale = value.at("scale");
      require(tensor.consistent() && scale > 0, "Tensor形状或scale不一致");
      return Data::tensor(std::move(tensor)).withTensorScale(scale);
    }
    if (kind == "Indices") {
      const auto key = text(value, "cloudKey");
      require(clouds.count(key), "Indices来源云未归档");
      Indices indices;
      indices.sourceCloudId = clouds.at(key).asCloud()->id;
      indices.values = read<std::int32_t>(value.at("values"));
      for (auto index : indices.values)
        require(index >= 0 && static_cast<std::size_t>(index) < pointCounts.at(key),
                "Indices引用越界");
      return Data::indices(std::move(indices));
    }
    if (kind == "Record") {
      Record record{text(value, "recordType"), value.at("data")};
      boundedJson(record.data);
      require(record.data.dump().size() <= MiB, "Record超过1MiB", "resource_limit");
      if (referenceTypes().count(record.type) && record.data.is_object())
        for (const char* field : {"sourceCloudId", "parentCloudId", "inputCloudId", "outputCloudId",
                                  "resultCloudId", "sliceCloudId", "cloudId", "modelCloudId"})
          if (record.data.contains(field)) {
            const auto key = record.data[field].get<std::string>();
            require(clouds.count(key), "Record引用的来源云未归档");
            record.data[field] = std::to_string(clouds.at(key).asCloud()->id);
          }
      return Data::record(std::move(record));
    }
    if (kind == "Measurement") {
      const auto values = read<double>(value.at("values"));
      require(values.size() == 4, "Measurement数值载荷无效");
      Measurement m;
      m.value = values[0];
      m.nominal = values[1];
      m.upper = values[2];
      m.lower = values[3];
      m.ok = value.at("ok");
      m.hasLimits = value.at("hasLimits");
      m.unit = text(value, "unit", 128);
      m.message = text(value, "message", 65536);
      m.verdict = text(value, "verdict", 64);
      return Data::measurement(std::move(m));
    }
    if (kind.rfind("Bundle<", 0) == 0) {
      Bundle bundle(text(value, "bundleKind"));
      require(bundle.typeName == kind && value.at("fields").is_array() &&
                  value.at("fields").size() <= 4096,
              "Bundle类型或字段无效");
      std::set<std::string> names;
      for (const auto& field : value.at("fields")) {
        const auto name = text(field, "name");
        require(!name.empty() && names.insert(name).second, "Bundle字段名重复或为空");
        bundle.set(name, decode(field.at("value"), depth + 1));
      }
      return Data::bundle(std::move(bundle));
    }
    if (kind == "Error") {
      const auto& s = value.at("status");
      const int phase = s.at("phase");
      require(phase >= 0 && phase <= 2, "诊断阶段无效");
      auto status =
          Status::Error(static_cast<Phase>(phase), text(s, "code"), text(s, "message", 65536),
                        text(s, "paramPath"), text(s, "portName"));
      status.ok = s.at("ok");
      return Data::error(std::move(status));
    }
    const auto values = read<float>(value.at("values"));
    auto count = [&](std::size_t expected) {
      require(values.size() == expected, "几何数值载荷长度不符");
    };
    if (kind == "Transform") {
      count(16);
      Transform t;
      std::copy(values.begin(), values.end(), t.m);
      return Data::transform(t);
    }
    if (kind == "Plane") {
      count(4);
      Plane p;
      std::copy(values.begin(), values.begin() + 3, p.normal);
      p.d = values[3];
      return Data::plane(p);
    }
    const int unit = value.at("unit");
    require(unit == 0 || unit == 1, "二维单位必须m或px");
    if (kind == "Point2D") {
      count(2);
      return Data::point2d({{values[0], values[1]}, static_cast<Unit2D>(unit)});
    }
    if (kind == "Circle2D") {
      count(3);
      return Data::circle2d({{values[0], values[1]}, values[2], static_cast<Unit2D>(unit)});
    }
    if (kind == "Box2D") {
      count(4);
      return Data::box2d(
          {{values[0], values[1]}, {values[2], values[3]}, static_cast<Unit2D>(unit)});
    }
    if (kind == "Line2D") {
      count(8);
      Line2D p;
      p.unit = static_cast<Unit2D>(unit);
      p.hasSegment = value.at("hasSegment");
      std::copy(values.begin(), values.begin() + 2, p.point);
      std::copy(values.begin() + 2, values.begin() + 4, p.dir);
      std::copy(values.begin() + 4, values.begin() + 6, p.start);
      std::copy(values.begin() + 6, values.end(), p.end);
      return Data::line2d(p);
    }
    throw Failure("unsupported_type", "不支持的归档数据类型：" + kind);
  }
};

void little(std::ostream& file, std::uint32_t n) {
  const char bytes[]{static_cast<char>(n), static_cast<char>(n >> 8), static_cast<char>(n >> 16),
                     static_cast<char>(n >> 24)};
  file.write(bytes, 4);
}
void save(const Frame& frame, const std::filesystem::path& path, bool overwrite) {
  const std::uint16_t endian = 1;
  require(*reinterpret_cast<const std::uint8_t*>(&endian) == 1, "仅支持小端归档平台",
          "unsupported_platform");
  Encoder encoder;
  Json manifest{{"format", "lyflow.FrameArchive"},
                {"version", 1},
                {"metadata", frame.metadata},
                {"entries", Json::array()},
                {"namedOutputs", Json::array()}};
  for (const auto& entry : frame.payload->entries)
    manifest["entries"].push_back(
        {{"nodeId", entry.nodeId}, {"port", entry.port}, {"data", encoder.encode(entry.data)}});
  for (const auto& name : frame.payload->named)
    manifest["namedOutputs"].push_back(
        {{"name", name.name}, {"nodeId", name.nodeId}, {"port", name.port}});
  auto validateReferences = [&](auto&& self, const Data& value) -> void {
    cancellation.check();
    if (const auto* indices = value.asIndices()) {
      const auto cloud = encoder.clouds.find(indices->sourceCloudId);
      require(cloud != encoder.clouds.end(), "Indices来源云未归档", "missing_reference");
      for (const auto i : indices->values)
        require(i >= 0 && static_cast<std::size_t>(i) < cloud->second->pointCount(),
                "Indices引用越界");
    }
    if (const auto* record = value.asRecord(); record && referenceTypes().count(record->type))
      for (const char* field : {"sourceCloudId", "parentCloudId", "inputCloudId", "outputCloudId",
                                "resultCloudId", "sliceCloudId", "cloudId", "modelCloudId"})
        if (record->data.contains(field))
          require(encoder.clouds.count(decimal(record->data.at(field), field)) != 0,
                  "Record来源云未归档", "missing_reference");
    if (const auto* bundle = value.asBundle())
      for (const auto& field : bundle->fields)
        self(self, field.second);
  };
  for (const auto& entry : frame.payload->entries)
    validateReferences(validateReferences, entry.data);
  manifest["payloadBytes"] = encoder.bytes;
  const auto header = manifest.dump();
  require(header.size() <= MiB, "归档清单超过1MiB", "resource_limit");
  require(!path.empty() &&
              std::filesystem::is_directory(path.parent_path().empty() ? "." : path.parent_path()),
          "归档目录不存在", "io");
  cancellation.check();
  const auto temp = path.parent_path() / std::filesystem::u8path(unique(".lyframe-pending-"));
  bool ownsTemp = false;
  try {
#if defined(_WIN32)
    const int fd =
        _wopen(temp.c_str(), _O_BINARY | _O_WRONLY | _O_CREAT | _O_EXCL, _S_IREAD | _S_IWRITE);
    FILE* raw = fd < 0 ? nullptr : _fdopen(fd, "wb");
    if (fd >= 0 && !raw)
      _close(fd);
#else
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    FILE* raw = fd < 0 ? nullptr : fdopen(fd, "wb");
    if (fd >= 0 && !raw)
      ::close(fd);
#endif
    ownsTemp = fd >= 0;
    require(raw != nullptr, "无法独占创建归档临时文件", "io");
    std::unique_ptr<FILE, decltype(&std::fclose)> file(raw, &std::fclose);
    auto write = [&](const void* data, std::size_t size) {
      const auto* source = static_cast<const std::uint8_t*>(data);
      for (std::size_t position = 0; position < size;) {
        cancellation.check();
        const auto count = std::min<std::size_t>(MiB, size - position);
        require(std::fwrite(source + position, 1, count, file.get()) == count, "归档写入失败",
                "io");
        position += count;
      }
    };
    write("LYFR\1\0\0\0", 8);
    const auto n = static_cast<std::uint32_t>(header.size());
    const std::uint8_t length[]{static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(n >> 8),
                                static_cast<std::uint8_t>(n >> 16),
                                static_cast<std::uint8_t>(n >> 24)};
    write(length, 4);
    write(header.data(), header.size());
    for (const auto& chunk : encoder.chunks)
      write(chunk.data, chunk.size);
    require(std::fflush(file.get()) == 0, "归档flush失败", "io");
    require(std::fclose(file.release()) == 0, "归档close失败", "io");
    cancellation.check();
#if defined(_WIN32)
    require(MoveFileExW(temp.c_str(), path.c_str(),
                        MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)) != 0,
            "归档目标冲突或移动失败", "io");
#else
    if (overwrite)
      std::filesystem::rename(temp, path);
    else {
      require(::link(temp.c_str(), path.c_str()) == 0, "归档目标已存在或链接失败", "io");
      std::filesystem::remove(temp);
    }
#endif
  } catch (...) {
    std::error_code ignored;
    if (ownsTemp)
      std::filesystem::remove(temp, ignored);
    throw;
  }
}
std::shared_ptr<Frame> load(const std::filesystem::path& path, std::uint64_t limit) {
  require(limit > 0 && limit <= maxFrameBytes, "读取预算必须在1..2GiB内", "resource_limit");
  cancellation.check();
  const auto size = std::filesystem::file_size(path);
  require(size >= 12 && size <= limit + MiB + 12, "归档大小超限或头被截断", "resource_limit");
  std::ifstream file(path, std::ios::binary);
  char magic[8];
  unsigned char length[4];
  file.read(magic, 8);
  file.read(reinterpret_cast<char*>(length), 4);
  require(file.good() && std::memcmp(magic, "LYFR\1\0\0\0", 8) == 0, "归档magic或版本无效",
          "unsupported_version");
  const std::uint32_t headerSize = std::uint32_t(length[0]) | (std::uint32_t(length[1]) << 8) |
                                   (std::uint32_t(length[2]) << 16) |
                                   (std::uint32_t(length[3]) << 24);
  require(headerSize > 0 && headerSize <= MiB && std::uint64_t(headerSize) + 12 <= size,
          "归档清单长度无效");
  std::string header(headerSize, '\0');
  file.read(header.data(), headerSize);
  require(file.good(), "归档清单被截断", "io");
  const auto manifest = Json::parse(header);
  require(manifest.at("format") == "lyflow.FrameArchive" && manifest.at("version") == 1,
          "归档格式版本不支持", "unsupported_version");
  const auto payloadBytes = manifest.at("payloadBytes").get<std::uint64_t>();
  require(payloadBytes <= limit && std::uint64_t(headerSize) + 12 + payloadBytes == size,
          "归档载荷长度不一致");
  const auto& entries = manifest.at("entries");
  require(entries.is_array() && entries.size() <= maxEntries &&
              manifest.at("namedOutputs").is_array() && manifest.at("namedOutputs").size() <= 4096,
          "归档端口数量超限", "resource_limit");
  std::vector<std::pair<std::uint64_t, std::uint64_t>> declaredRanges;
  auto preflight = [&](auto&& self, const Json& item, std::size_t depth) -> void {
    require(depth <= 24, "归档JSON嵌套超过24层", "resource_limit");
    cancellation.check();
    if (item.is_object() && item.contains("offset") && item.contains("bytes")) {
      require(item["offset"].is_number_unsigned() && item["bytes"].is_number_unsigned(),
              "二进制范围必须为无符号整数");
      const auto start = item["offset"].get<std::uint64_t>(),
                 size = item["bytes"].get<std::uint64_t>();
      require(start <= payloadBytes && size <= payloadBytes - start, "二进制范围越界");
      if (size)
        declaredRanges.emplace_back(start, start + size);
      return;
    }
    if (item.is_object() && item.value("kind", std::string{}) == "Record")
      return;
    if (item.is_object() || item.is_array())
      for (const auto& child : item)
        self(self, child, depth + 1);
  };
  for (const auto& entry : entries)
    preflight(preflight, entry.at("data"), 0);
  std::sort(declaredRanges.begin(), declaredRanges.end());
  std::uint64_t preflightEnd = 0;
  for (const auto& range : declaredRanges) {
    require(range.first == preflightEnd, "载荷范围存在空洞或重叠");
    preflightEnd = range.second;
  }
  require(preflightEnd == payloadBytes, "载荷含未声明数据");
  auto data = std::make_shared<Payload>();
  data->reserve(payloadBytes);
  Decoder decoder{file, std::uint64_t(headerSize) + 12, payloadBytes};
  for (const auto& entry : entries)
    decoder.scanClouds(entry.at("data"));
  std::set<std::pair<std::string, std::string>> addresses;
  for (const auto& entry : entries) {
    const auto node = text(entry, "nodeId", 4096), port = text(entry, "port", 4096);
    require(!node.empty() && !port.empty() && addresses.emplace(node, port).second,
            "归档端口地址重复或为空");
    data->entries.push_back({node, port, decoder.decode(entry.at("data"))});
  }
  std::sort(decoder.ranges.begin(), decoder.ranges.end());
  std::uint64_t end = 0;
  for (const auto& range : decoder.ranges) {
    require(range.first == end, "载荷范围存在空洞或重叠");
    end = range.second;
  }
  require(end == payloadBytes, "载荷包含未声明的尾部数据");
  std::set<std::string> names;
  for (const auto& name : manifest.at("namedOutputs")) {
    auto n = text(name, "name", 4096), node = text(name, "nodeId", 4096),
         port = text(name, "port", 4096);
    require(!n.empty() && names.insert(n).second, "命名输出重复或为空");
    data->named.push_back({n, node, port});
  }
  data->account(limit);
  cancellation.check();
  auto result = std::make_shared<Frame>(data, metadata(manifest.at("metadata"), true));
  for (const auto& name : data->named) {
    Data output;
    require(exec::ResultStore::instance().get(result->view, name.nodeId, name.port, output),
            "归档命名输出未绑定实际数据");
  }
  return result;
}

struct Session {
  mutable std::mutex mutex;
  Json options;
  std::string id, epoch = unique("session-");
  std::deque<std::shared_ptr<Frame>> queue;
  std::uint64_t bytes = 0, next = 1, lastTimestamp = 0, dropped = 0;
  bool hasTimestamp = false;
  std::size_t capacity;
  std::uint64_t maxBytes;
  std::string overflow, order, schemaChange;
  Json signature;
  explicit Session(Json settings) : options(std::move(settings)) {
    require(options.is_object(), "会话配置必须为对象");
    id = text(options, "id");
    require(!id.empty(), "会话id不得为空");
    capacity = options.value("capacity", std::size_t{16});
    maxBytes = options.value("maxBytes", std::uint64_t{256 * MiB});
    overflow = options.value("overflow", std::string("drop_oldest"));
    order = options.value("timestampOrder", std::string("nondecreasing"));
    schemaChange = options.value("schemaChange", std::string("reset"));
    require(capacity > 0 && capacity <= 1024 && maxBytes > 0 && maxBytes <= maxFrameBytes,
            "会话容量或字节预算无效", "resource_limit");
    require(overflow == "drop_oldest" || overflow == "reject" || overflow == "drop_newest",
            "未知满队列策略");
    require(order == "arrival" || order == "nondecreasing", "未知时间顺序策略");
    require(schemaChange == "reset" || schemaChange == "reject", "未知类型改变策略");
  }
  Json info() const {
    return {{"type", "lyflow.Session.v1"},
            {"schemaVersion", 1},
            {"id", id},
            {"epoch", epoch},
            {"size", queue.size()},
            {"bytes", bytes},
            {"capacity", capacity},
            {"maxBytes", maxBytes},
            {"nextSequence", std::to_string(next)},
            {"dropped", dropped},
            {"overflow", overflow},
            {"timestampOrder", order},
            {"schemaChange", schemaChange}};
  }
  void reset() {
    auto newEpoch = unique("session-");
    queue.clear();
    bytes = 0;
    lastTimestamp = 0;
    hasTimestamp = false;
    signature = nullptr;
    epoch.swap(newEpoch);
  }
  Json push(const Frame& input, bool explicitReset) {
    const auto timestamp = decimal(input.metadata.at("timestampNs"), "timestampNs");
    require(input.payload->bytes <= maxBytes, "单帧超过会话字节预算", "resource_limit");
    Json types = Json::array();
    for (const auto& name : input.payload->named) {
      Data data;
      require(exec::ResultStore::instance().get(input.view, name.nodeId, name.port, data),
              "帧输出不可用");
      types.push_back({{"name", name.name},
                       {"type", data.typeName()},
                       {"recordType", data.asRecord() ? data.asRecord()->type : std::string{}}});
    }
    if (input.payload->named.empty())
      for (const auto& entry : input.payload->entries)
        types.push_back(
            {{"name", entry.nodeId + "." + entry.port},
             {"type", entry.data.typeName()},
             {"recordType", entry.data.asRecord() ? entry.data.asRecord()->type : std::string{}}});
    std::sort(types.begin(), types.end(),
              [](const Json& a, const Json& b) { return a["name"] < b["name"]; });
    types = Json{{"ports", std::move(types)},
                 {"frame", input.metadata.at("frame")},
                 {"calibrationVersion", input.metadata.at("calibrationVersion")}};
    const bool changed = !signature.is_null() && signature != types;
    require(!changed || explicitReset || schemaChange == "reset", "队列数据类型变化需显式reset",
            "type_changed");
    const bool clear = explicitReset || changed;
    require(clear || order == "arrival" || !hasTimestamp || timestamp >= lastTimestamp,
            "帧时间戳倒序", "out_of_order");
    require(next != std::numeric_limits<std::uint64_t>::max(), "会话序列耗尽", "resource_limit");
    const bool full =
        !clear && (queue.size() >= capacity || bytes > maxBytes - input.payload->bytes);
    if (full && overflow != "drop_oldest")
      return {{"ok", overflow == "drop_newest"},
              {"accepted", false},
              {"code", "queue_full"},
              {"sequence", nullptr},
              {"dropped", 0}};
    auto context = input.metadata;
    auto newEpoch = clear ? unique("session-") : epoch;
    context["session"] = {{"id", id}, {"epoch", newEpoch}, {"sequence", std::to_string(next)}};
    auto frame = std::make_shared<Frame>(input.payload, std::move(context));
    auto candidate = clear ? decltype(queue){} : queue;
    auto candidateBytes = clear ? std::uint64_t{0} : bytes;
    std::size_t removed = 0;
    while (!candidate.empty() &&
           (candidate.size() >= capacity || candidateBytes > maxBytes - frame->payload->bytes)) {
      candidateBytes -= candidate.front()->payload->bytes;
      candidate.pop_front();
      ++removed;
    }
    candidate.push_back(std::move(frame));
    candidateBytes += input.payload->bytes;
    cancellation.check();
    Json receipt{{"ok", true},         {"accepted", true}, {"sequence", std::to_string(next)},
                 {"dropped", removed}, {"reset", clear},   {"epoch", newEpoch},
                 {"sessionId", id}};
    queue.swap(candidate);
    epoch.swap(newEpoch);
    bytes = candidateBytes;
    signature = std::move(types);
    lastTimestamp = timestamp;
    hasTimestamp = true;
    dropped += removed;

    ++next;
    return receipt;
  }
};

char* copyString(const std::string& value) noexcept {
  auto* out = static_cast<char*>(std::malloc(value.size() + 1));
  if (out) {
    std::memcpy(out, value.data(), value.size());
    out[value.size()] = 0;
  }
  return out;
}
char* diagnostic(const char* code, const char* message) noexcept {
  try {
    return copyString(Json{{"code", code}, {"message", message}}.dump(
        -1, ' ', false, Json::error_handler_t::replace));
  } catch (...) {
    return nullptr;
  }
}
template <class F> char* status(F&& operation) noexcept {
  try {
    return copyString(operation());
  } catch (const Failure& error) {
    return diagnostic(error.code.c_str(), error.what());
  } catch (const std::bad_alloc&) {
    return diagnostic("resource_limit", "内存不足");
  } catch (const std::exception& error) {
    return diagnostic("bad_input", error.what());
  } catch (...) {
    return diagnostic("internal", "帧操作失败");
  }
}
} // namespace lyflow::runtime

struct lyflow_frame {
  std::shared_ptr<lyflow::runtime::Frame> value;
};
struct lyflow_session {
  std::shared_ptr<lyflow::runtime::Session> value;
};

extern "C" {
char* lyflow_runtime_capabilities() {
  return lyflow::runtime::status([] {
    return std::string(
        "{\"schemaVersion\":1,\"frames\":true,\"sessions\":true,\"resultBindings\":true,\"cloudSampling\":\"ordered-stride-v1\",\"profiles\":true,\"profileTransfer\":\"ordered-slots-v1\"}");
  });
}
char* lyflow_run_bindings(const char* run) {
  return lyflow::runtime::status(
      [&] { return lyflow::exec::ResultStore::instance().bindingsJson(run ? run : ""); });
}
char* lyflow_frame_capture(const char* run, const char* metadata, const char* options,
                           lyflow_frame** out) {
  if (out)
    *out = nullptr;
  return lyflow::runtime::status([&] {
    lyflow::runtime::require(out && run, "帧输出或运行标识为空");
    auto value = lyflow::runtime::capture(run, lyflow::runtime::parse(metadata),
                                          lyflow::runtime::parse(options));
    *out = new lyflow_frame{std::move(value)};
    return std::string{};
  });
}
char* lyflow_frame_load(const char* path, std::uint64_t maxBytes, lyflow_frame** out) {
  return lyflow_frame_load_ex(path, maxBytes, nullptr, nullptr, out);
}
char* lyflow_frame_load_ex(const char* path, std::uint64_t maxBytes, lyflow_cancel_cb cb,
                           void* user, lyflow_frame** out) {
  lyflow::runtime::CancelScope scope(cb, user);
  if (out)
    *out = nullptr;
  return lyflow::runtime::status([&] {
    lyflow::runtime::require(out && path && *path, "归档路径或帧输出为空");
    auto value = lyflow::runtime::load(std::filesystem::u8path(path),
                                       maxBytes ? maxBytes : 1024 * lyflow::runtime::MiB);
    *out = new lyflow_frame{std::move(value)};
    return std::string{};
  });
}
char* lyflow_frame_save(const lyflow_frame* frame, const char* path, std::int32_t overwrite) {
  return lyflow_frame_save_ex(frame, path, overwrite, nullptr, nullptr);
}
char* lyflow_frame_save_ex(const lyflow_frame* frame, const char* path, std::int32_t overwrite,
                           lyflow_cancel_cb cb, void* user) {
  lyflow::runtime::CancelScope scope(cb, user);
  return lyflow::runtime::status([&] {
    lyflow::runtime::require(frame && frame->value && path && *path &&
                                 (overwrite == 0 || overwrite == 1),
                             "帧或归档路径无效");
    lyflow::runtime::save(*frame->value, std::filesystem::u8path(path), overwrite != 0);
    return std::string{};
  });
}
char* lyflow_frame_metadata(const lyflow_frame* frame) {
  return lyflow::runtime::status([&] {
    lyflow::runtime::require(frame && frame->value, "帧为空");
    return frame->value->info().dump();
  });
}
const char* lyflow_frame_run_id(const lyflow_frame* frame) {
  return frame && frame->value ? frame->value->view.c_str() : nullptr;
}
void lyflow_frame_free(lyflow_frame* frame) { delete frame; }
char* lyflow_session_create(const char* options, lyflow_session** out) {
  if (out)
    *out = nullptr;
  return lyflow::runtime::status([&] {
    lyflow::runtime::require(out, "会话输出为空");
    auto value = std::make_shared<lyflow::runtime::Session>(lyflow::runtime::parse(options));
    *out = new lyflow_session{std::move(value)};
    return std::string{};
  });
}
char* lyflow_session_push(lyflow_session* session, const lyflow_frame* frame, std::int32_t reset) {
  return lyflow_session_push_ex(session, frame, reset, nullptr, nullptr);
}
char* lyflow_session_push_ex(lyflow_session* session, const lyflow_frame* frame, std::int32_t reset,
                             lyflow_cancel_cb cb, void* user) {
  lyflow::runtime::CancelScope scope(cb, user);
  return lyflow::runtime::status([&] {
    lyflow::runtime::require(session && session->value && frame && frame->value &&
                                 (reset == 0 || reset == 1),
                             "会话或帧为空");
    std::lock_guard<std::mutex> lock(session->value->mutex);
    return session->value->push(*frame->value, reset != 0).dump();
  });
}
char* lyflow_session_front(lyflow_session* session, std::int32_t remove, lyflow_frame** out) {
  if (out)
    *out = nullptr;
  return lyflow::runtime::status([&] {
    lyflow::runtime::require(session && session->value && out && (remove == 0 || remove == 1),
                             "会话或帧输出为空");
    std::lock_guard<std::mutex> lock(session->value->mutex);
    if (!session->value->queue.empty()) {
      auto value = session->value->queue.front();
      *out = new lyflow_frame{value};
      if (remove) {
        session->value->bytes -= value->payload->bytes;
        session->value->queue.pop_front();
      }
    }
    return std::string{};
  });
}
char* lyflow_session_reset(lyflow_session* session) {
  return lyflow::runtime::status([&] {
    lyflow::runtime::require(session && session->value, "会话为空");
    std::lock_guard<std::mutex> lock(session->value->mutex);
    session->value->reset();
    return std::string{};
  });
}
char* lyflow_session_info(const lyflow_session* session) {
  return lyflow::runtime::status([&] {
    lyflow::runtime::require(session && session->value, "会话为空");
    std::lock_guard<std::mutex> lock(session->value->mutex);
    return session->value->info().dump();
  });
}
void lyflow_session_free(lyflow_session* session) { delete session; }
}

namespace lyflow::runtime {
std::vector<exec::InjectedInput> frameInputs(const lyflow_frame* frame,
                                             const std::string& bindings) {
  require(frame && frame->value, "帧为空");
  require(bindings.size() <= MiB, "注入绑定超过预算", "resource_limit");
  const auto items = Json::parse(bindings);
  require(items.is_array() && items.size() <= maxEntries, "绑定必须为有界数组");
  std::vector<exec::InjectedInput> result;
  std::set<std::pair<std::string, std::string>> targets;
  for (const auto& item : items) {
    const auto node = text(item, "nodeId", 4096), port = text(item, "port", 4096);
    require(!node.empty() && !port.empty() && targets.emplace(node, port).second,
            "目标端口重复或为空");
    std::string sourceNode, sourcePort;
    if (item.contains("name")) {
      const auto name = text(item, "name", 4096);
      const auto& names = frame->value->payload->named;
      const auto found =
          std::find_if(names.begin(), names.end(), [&](const auto& o) { return o.name == name; });
      require(found != names.end(), "命名输出不存在");
      sourceNode = found->nodeId;
      sourcePort = found->port;
    } else {
      sourceNode = text(item, "sourceNode", 4096);
      sourcePort = text(item, "sourcePort", 4096);
    }
    Data value;
    require(exec::ResultStore::instance().get(frame->value->view, sourceNode, sourcePort, value),
            "源端口不在帧中");
    result.push_back({node, port, std::move(value)});
  }
  return result;
}
}

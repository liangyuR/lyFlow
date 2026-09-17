#pragma once
// 端口契约（ADR-0024）。四种键，刻意不做表达式语言：
//   elementCount: { eq | min | max }   点数 / 下标数 / 张量元素数
//   finite: true                       点云坐标、张量、Measurement 的值必须是有限的
//   shape: [..]                        张量形状，-1 是「这一维随便」
//   recordType: "<type>"               Record 的 type 字串
// 需要第五种的时候先问「是不是这个算子该拆了」，与 Param::visibleWhen 同一条原则。
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lyflow/data.h"
#include "lyflow/manifest.h"

namespace lyflow {

/// 契约声明本身合不合法。返回全部问题（每条一句人话），空 = 干净。
/// `where` 只用来拼错误信息，比如 "operator 'gap.x' input port 'primary'"。
/// 四种键之外的任何键都在这里被拒 —— manifest --check 与 startup 自检走的是同一个函数。
std::vector<std::string> validatePortContract(const nlohmann::json& contract,
                                              const std::string& where);

/// 一个到达端口的值符不符合契约。
/// **contract 为空对象时立刻返回 true，一次遍历都不做**（m6-plan §9 的「默认零开销」）。
/// 不符时填 expected / actual（原样进 summary 的 contractViolations）与一句
/// 带期望和实际的人话（进 Status::message）。
bool checkPortContract(const nlohmann::json& contract, const Data& value,
                       nlohmann::json& expected, nlohmann::json& actual, std::string& message);

}  // namespace lyflow

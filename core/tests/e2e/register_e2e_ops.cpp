// 编辑器 e2e 用的测试算子（param-recipe P2.10）。这个文件编进 core，但只在进程环境里
// LYFLOW_TEST_OPS=1 时才注册：e2e 跑的是真的 app（tauri dev），测试算子只能从这里进去；
// 正常启动的 app、CLI、dump 出来的 manifest 里都没有它们。scripts/e2e/harness.mjs 起 app 时设这个变量。
// 放在 core/tests/ 的子目录里，免得被 doctest 目标的 *.cpp 通配再编一份（它已经链着 core 的对象库）。
#include <cstdlib>
#include <cstring>

#include "param_showcase_op.h"

namespace lyflow::test {

void registerE2eOps(Registry& r) {
  const char* flag = std::getenv("LYFLOW_TEST_OPS");
  if (flag == nullptr || std::strcmp(flag, "1") != 0) return;
  registerParamShowcase(r);
}

}  // namespace lyflow::test

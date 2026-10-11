#include "lyflow/version.h"
#if defined(_WIN32)
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif

#if defined(LYFLOW_FIXTURE_legacy)
EXPORT unsigned lyflow_abi_version() { return 15; }
#else
EXPORT const char* lyflow_abi_version_string() {
#if defined(LYFLOW_FIXTURE_abi_patch)
  return LYFLOW_FIXTURE_OTHER_VERSION;
#elif defined(LYFLOW_FIXTURE_null_abi)
  return nullptr;
#else
  return LYFLOW_VERSION;
#endif
}
#endif

EXPORT const char* lyflow_version() {
#if defined(LYFLOW_FIXTURE_engine_patch)
  return LYFLOW_FIXTURE_OTHER_VERSION;
#else
  return LYFLOW_VERSION;
#endif
}

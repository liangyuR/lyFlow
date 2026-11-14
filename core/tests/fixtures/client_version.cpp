#include "lyflow/version.h"
#if defined(_WIN32)
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif

#if defined(_WIN32) && defined(LYFLOW_FIXTURE_matching)
extern "C" __declspec(dllimport) unsigned long __stdcall timeGetTime();
#endif

#if defined(LYFLOW_FIXTURE_legacy)
EXPORT unsigned lyflow_abi_version() { return 15; }
#else
EXPORT const char* lyflow_abi_version_string() {
#if defined(LYFLOW_FIXTURE_abi_patch)
  return LYFLOW_FIXTURE_OTHER_VERSION;
#elif defined(LYFLOW_FIXTURE_abi_minor)
  return LYFLOW_FIXTURE_OTHER_MINOR;
#elif defined(LYFLOW_FIXTURE_null_abi)
  return nullptr;
#else
  return LYFLOW_VERSION;
#endif
}
#endif

EXPORT const char* lyflow_version() {
#if defined(_WIN32) && defined(LYFLOW_FIXTURE_matching)
  (void)timeGetTime();
#endif
#if defined(LYFLOW_FIXTURE_engine_patch)
  return LYFLOW_FIXTURE_OTHER_VERSION;
#elif defined(LYFLOW_FIXTURE_engine_minor)
  return LYFLOW_FIXTURE_OTHER_MINOR;
#else
  return LYFLOW_VERSION;
#endif
}

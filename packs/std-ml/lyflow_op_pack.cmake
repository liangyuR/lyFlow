# LyFlow 标准算子包：Tensor 上的通用推理（ADR-0015）。
# 全仓库唯一一处 onnxruntime 依赖 —— 推理引擎是重依赖，不该和点云包绑死（T1）。

get_filename_component(LYFLOW_ML_REPO "${LYFLOW_PACK_DIR}/../.." ABSOLUTE)

set(LYFLOW_ORT_VERSION "1.19.2")
if(NOT DEFINED LYFLOW_ONNXRUNTIME_ROOT OR "${LYFLOW_ONNXRUNTIME_ROOT}" STREQUAL "")
  if(DEFINED ENV{LYFLOW_ONNXRUNTIME_ROOT} AND NOT "$ENV{LYFLOW_ONNXRUNTIME_ROOT}" STREQUAL "")
    set(LYFLOW_ONNXRUNTIME_ROOT "$ENV{LYFLOW_ONNXRUNTIME_ROOT}")
  else()
    set(LYFLOW_ONNXRUNTIME_ROOT
        "${LYFLOW_ML_REPO}/third_party/onnxruntime/onnxruntime-win-x64-${LYFLOW_ORT_VERSION}")
  endif()
endif()
file(TO_CMAKE_PATH "${LYFLOW_ONNXRUNTIME_ROOT}" LYFLOW_ONNXRUNTIME_ROOT)

file(GLOB STD_ML_SOURCES CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/ops/*.cpp")
file(GLOB STD_ML_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")

lyflow_op_pack(
  NAME         std-ml
  VERSION      0.1.0
  DEFAULT      ON
  SOURCES      ${STD_ML_SOURCES}
  TEST_SOURCES ${STD_ML_TESTS}
  INCLUDES     "${LYFLOW_ONNXRUNTIME_ROOT}/include"
  LINK         "${LYFLOW_ONNXRUNTIME_ROOT}/lib/onnxruntime.lib"
)

if(LYFLOW_PACK_ENABLED)
  # 不静默跳过：缺了就把准备命令打出来（T9）。
  if(NOT EXISTS "${LYFLOW_ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
    message(FATAL_ERROR
      "packs/std-ml 要 onnxruntime ${LYFLOW_ORT_VERSION}，而 ${LYFLOW_ONNXRUNTIME_ROOT} 里没有。\n"
      "先跑：powershell -ExecutionPolicy Bypass -File scripts/fetch-onnxruntime.ps1\n"
      "或设 LYFLOW_ONNXRUNTIME_ROOT 指向已有的一份。")
  endif()
  # 不在 C:\vcpkg 里，vcpkg 的 applocal 看不见它们；bridge/build.rs 整目录搬走 bin/。
  file(COPY "${LYFLOW_ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
            "${LYFLOW_ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.dll"
       DESTINATION "${CMAKE_BINARY_DIR}/bin")
endif()

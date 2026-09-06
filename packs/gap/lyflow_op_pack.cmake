file(GLOB_RECURSE GAP_ALGO_SOURCES CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/algo/*.cpp")
file(GLOB GAP_OP_SOURCES CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/ops/*.cpp")
file(GLOB GAP_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")

# onnxruntime 与 std-ml 用同一份（那个包已经把根目录解析好了）。
find_package(yaml-cpp CONFIG QUIET)

lyflow_op_pack(
  NAME     gap
  VERSION  0.2.0
  DEFAULT  OFF
  SOURCES  ${GAP_OP_SOURCES} ${GAP_ALGO_SOURCES}
  TEST_SOURCES ${GAP_TESTS}
  PCH      "${LYFLOW_PACK_DIR}/ops/gap_pch.h"
  INCLUDES "${LYFLOW_PACK_DIR}/algo" "${LYFLOW_ONNXRUNTIME_ROOT}/include"
  LINK     lyflow_pcl_support lyflow_std_algo yaml-cpp::yaml-cpp
           "${LYFLOW_ONNXRUNTIME_ROOT}/lib/onnxruntime.lib"
  DEFINES  _USE_MATH_DEFINES
  # yaml-cpp 的 YAML::Exception 继承 std::runtime_error 又带 dllexport，不是包能改的
  OPTIONS  /wd4275
)

if(LYFLOW_PACK_ENABLED)
  if(NOT TARGET lyflow_std_algo)
    message(FATAL_ERROR
      "gap 包要 lyflow_std_algo —— 它由 packs/std-pointcloud 提供，别把 LYFLOW_STD_PACKS 关掉。")
  endif()
  if(NOT TARGET yaml-cpp::yaml-cpp)
    message(FATAL_ERROR
      "gap 包要 yaml-cpp。先跑：C:\\vcpkg\\vcpkg.exe install yaml-cpp:x64-windows")
  endif()
  if(NOT EXISTS "${LYFLOW_ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
    message(FATAL_ERROR
      "gap 包要 onnxruntime，而 ${LYFLOW_ONNXRUNTIME_ROOT} 里没有。\n"
      "先跑：powershell -ExecutionPolicy Bypass -File scripts/fetch-onnxruntime.ps1")
  endif()
  # yaml-cpp 的 DLL 在 vcpkg 里，applocal 会带；onnxruntime 的由 std-ml 包拷。
endif()

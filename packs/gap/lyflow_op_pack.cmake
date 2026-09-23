file(GLOB_RECURSE GAP_ALGO_SOURCES CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/algo/*.cpp")
file(GLOB GAP_OP_SOURCES CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/ops/*.cpp")
file(GLOB GAP_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")

# onnxruntime 与 std-ml 用同一份（那个包已经把根目录解析好了）。
find_package(yaml-cpp CONFIG QUIET)

# 随包的片段（m8-plan L14）：snippets/*.lyflow-snippet.json 编进包里，经 manifest 的 snippets 段
# 给出（ops/snippets.cpp 注册）。按字节写成十六进制数组：不受 MSVC 字符串字面量长度的限制，
# 也不经过源文件编码。内容一改就重新 configure。
file(GLOB GAP_SNIPPETS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/snippets/*.lyflow-snippet.json")
list(SORT GAP_SNIPPETS)
set(GAP_GENERATED "${CMAKE_BINARY_DIR}/generated/gap")
set(_gap_inc "// 由 packs/gap/lyflow_op_pack.cmake 从 snippets/*.lyflow-snippet.json 生成，勿手改。\n")
set(_gap_table "")
set(_gap_i 0)
foreach(_f IN LISTS GAP_SNIPPETS)
  file(READ "${_f}" _hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _hex "${_hex}")
  get_filename_component(_name "${_f}" NAME)
  string(APPEND _gap_inc "static const unsigned char kSnippet${_gap_i}[] = {${_hex}};\n")
  string(APPEND _gap_table "    {\"${_name}\", kSnippet${_gap_i}, sizeof(kSnippet${_gap_i})},\n")
  math(EXPR _gap_i "${_gap_i} + 1")
endforeach()
string(APPEND _gap_inc "static const SnippetBlob kSnippetBlobs[] = {\n${_gap_table}    {nullptr, nullptr, 0},\n};\n")
file(WRITE "${GAP_GENERATED}/gap_snippets.inc.in" "${_gap_inc}")
configure_file("${GAP_GENERATED}/gap_snippets.inc.in" "${GAP_GENERATED}/gap_snippets.inc" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GAP_SNIPPETS})

lyflow_op_pack(
  NAME     gap
  VERSION  0.2.0
  DEFAULT  OFF
  SOURCES  ${GAP_OP_SOURCES} ${GAP_ALGO_SOURCES}
  TEST_SOURCES ${GAP_TESTS}
  PCH      "${LYFLOW_PACK_DIR}/ops/gap_pch.h"
  INCLUDES "${LYFLOW_PACK_DIR}/algo" "${LYFLOW_ONNXRUNTIME_ROOT}/include" "${GAP_GENERATED}"
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

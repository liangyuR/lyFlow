file(GLOB_RECURSE GAP_ALGO_SOURCES CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/algo/*.cpp")
file(GLOB GAP_OP_SOURCES CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/ops/*.cpp")
file(GLOB GAP_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")

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
  INCLUDES "${LYFLOW_PACK_DIR}/algo" "${GAP_GENERATED}"
  LINK     lyflow_pcl_support lyflow_std_algo lyflow_yaml_support lyflow_onnx_support
  # _ENABLE_EXTENDED_ALIGNED_STORAGE：groove.cpp 对 pcl::PointXYZRGB（16 字节对齐）做 std::stable_sort，
  # VS2019（MSVC 14.29）的实现借 aligned_union 开临时缓冲，遇到扩展对齐就 static_assert。定这个宏是认可
  # 15.8 之后正确的对齐 —— 只在与 15.8 之前编出的代码混链时才有布局差异，core 整个是同一个工具链编的
  DEFINES  _USE_MATH_DEFINES _ENABLE_EXTENDED_ALIGNED_STORAGE
  # yaml-cpp 的 YAML::Exception 继承 std::runtime_error 又带 dllexport，不是包能改的
  OPTIONS  /wd4275
)

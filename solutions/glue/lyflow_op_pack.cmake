file(GLOB GLUE_SOURCES CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/ops/*.cpp" "${LYFLOW_PACK_DIR}/algo/*.cpp")
file(GLOB GLUE_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")

# 随包的片段（m8-plan L14）：snippets/*.lyflow-snippet.json 按字节编成十六进制数组（照 packs/gap），
# 不受 MSVC 字符串字面量长度的限制，也不经过源文件编码。内容一改就重新 configure。
file(GLOB GLUE_SNIPPETS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/snippets/*.lyflow-snippet.json")
list(SORT GLUE_SNIPPETS)
set(GLUE_GENERATED "${CMAKE_BINARY_DIR}/generated/glue")
set(_glue_inc "// 由 solutions/glue/lyflow_op_pack.cmake 从 snippets/*.lyflow-snippet.json 生成，勿手改。\n")
set(_glue_table "")
set(_glue_i 0)
foreach(_f IN LISTS GLUE_SNIPPETS)
  file(READ "${_f}" _hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _hex "${_hex}")
  get_filename_component(_name "${_f}" NAME)
  string(APPEND _glue_inc "static const unsigned char kSnippet${_glue_i}[] = {${_hex}};\n")
  string(APPEND _glue_table "    {\"${_name}\", kSnippet${_glue_i}, sizeof(kSnippet${_glue_i})},\n")
  math(EXPR _glue_i "${_glue_i} + 1")
endforeach()
string(APPEND _glue_inc "static const SnippetBlob kSnippetBlobs[] = {\n${_glue_table}    {nullptr, nullptr, 0},\n};\n")
file(WRITE "${GLUE_GENERATED}/glue_snippets.inc.in" "${_glue_inc}")
configure_file("${GLUE_GENERATED}/glue_snippets.inc.in" "${GLUE_GENERATED}/glue_snippets.inc" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GLUE_SNIPPETS})

lyflow_op_pack(
  NAME         solution-glue
  VERSION      0.4.0
  DEFAULT      OFF
  SOURCES      ${GLUE_SOURCES}
  TEST_SOURCES ${GLUE_TESTS}
  PCH          "${LYFLOW_PACK_DIR}/ops/glue_pch.h"
  INCLUDES     "${LYFLOW_PACK_DIR}" "${GLUE_GENERATED}"
  LINK         lyflow_opencv_support lyflow_geometry_kernel
)

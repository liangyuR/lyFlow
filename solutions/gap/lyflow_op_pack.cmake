set(GAP_OP_SOURCES
  "${LYFLOW_PACK_DIR}/ops/register.cpp"
  "${LYFLOW_PACK_DIR}/ops/common.cpp"
  "${LYFLOW_PACK_DIR}/ops/result_bundle.cpp"
  "${LYFLOW_PACK_DIR}/ops/snippets.cpp"
  "${LYFLOW_PACK_DIR}/ops/fit.cpp"
  "${LYFLOW_PACK_DIR}/ops/measure.cpp"
  "${LYFLOW_PACK_DIR}/ops/groove.cpp"
  "${LYFLOW_PACK_DIR}/ops/notch.cpp"
  "${LYFLOW_PACK_DIR}/ops/profile_scene.cpp"
  "${LYFLOW_PACK_DIR}/ops/profile_scan.cpp"
  "${LYFLOW_PACK_DIR}/ops/profile_guard.cpp"
  "${LYFLOW_PACK_DIR}/ops/profile_regions.cpp"
  "${LYFLOW_PACK_DIR}/ops/profile_bundle_ops.cpp"
  "${LYFLOW_PACK_DIR}/ops/profile_feature_bundle.cpp"
  "${LYFLOW_PACK_DIR}/ops/profile_model.cpp"
  "${LYFLOW_PACK_DIR}/ops/profile_model_ops.cpp"
  "${LYFLOW_PACK_DIR}/ops/profile_circles.cpp"
  "${LYFLOW_PACK_DIR}/ops/profile_measurement_ops.cpp"
  "${LYFLOW_PACK_DIR}/ops/profile_inspection.cpp")
set(GAP_TESTS
  "${LYFLOW_PACK_DIR}/tests/test_profile_assets.cpp"
  "${LYFLOW_PACK_DIR}/tests/test_profile_scene.cpp"
  "${LYFLOW_PACK_DIR}/tests/test_profile_feature_bundle.cpp"
  "${LYFLOW_PACK_DIR}/tests/test_profile_result_bundle.cpp"
  "${LYFLOW_PACK_DIR}/tests/test_profile_scan.cpp"
  "${LYFLOW_PACK_DIR}/tests/test_profile_guard.cpp"
  "${LYFLOW_PACK_DIR}/tests/test_profile_regions.cpp"
  "${LYFLOW_PACK_DIR}/tests/test_profile_model.cpp"
  "${LYFLOW_PACK_DIR}/tests/test_profile_model_ops.cpp"
  "${LYFLOW_PACK_DIR}/tests/test_profile_circles.cpp"
  "${LYFLOW_PACK_DIR}/tests/test_profile_measurement_ops.cpp"
  "${LYFLOW_PACK_DIR}/tests/test_profile_inspection.cpp")
set(_gap_links lyflow_gap_geometry lyflow_gap_model_adapters lyflow_profile_contract lyflow_metrology_kernel)
set(_gap_defines _USE_MATH_DEFINES _ENABLE_EXTENDED_ALIGNED_STORAGE)
if(TARGET lyflow_gap_template)
  list(APPEND GAP_OP_SOURCES
    "${LYFLOW_PACK_DIR}/ops/profile_template.cpp"
    "${LYFLOW_PACK_DIR}/ops/profile_locate_template.cpp")
  list(APPEND GAP_TESTS
    "${LYFLOW_PACK_DIR}/tests/test_profile_template.cpp"
    "${LYFLOW_PACK_DIR}/tests/test_profile_locate_template.cpp")
  list(APPEND _gap_links lyflow_gap_template)
  list(APPEND _gap_defines LYFLOW_GAP_TEMPLATE=1)
endif()
if(TARGET lyflow_gap_model)
  list(APPEND _gap_links lyflow_gap_model)
  list(APPEND _gap_defines LYFLOW_GAP_MODEL=1)
  set_property(SOURCE "${LYFLOW_PACK_DIR}/tests/test_profile_model_ops.cpp" APPEND
    PROPERTY COMPILE_DEFINITIONS LYFLOW_GAP_MODEL=1)
endif()
if(TARGET lyflow_gap_import)
  list(APPEND GAP_OP_SOURCES "${LYFLOW_PACK_DIR}/ops/import_standard_gap.cpp")
  list(APPEND GAP_TESTS "${LYFLOW_PACK_DIR}/tests/test_profile_graph_upgrade.cpp")
  if(TARGET lyflow_gap_template AND TARGET lyflow_gap_model)
    list(APPEND GAP_TESTS "${LYFLOW_PACK_DIR}/tests/test_import_bundle.cpp")
  endif()
  list(APPEND _gap_links lyflow_gap_import)
  list(APPEND _gap_defines LYFLOW_GAP_IMPORT=1)
endif()

foreach(_gap_test IN LISTS GAP_TESTS)
  foreach(_gap_cap TEMPLATE MODEL IMPORT COMPAT)
    string(TOLOWER "${_gap_cap}" _gap_cap_name)
    if(TARGET lyflow_gap_${_gap_cap_name})
      set_property(SOURCE "${_gap_test}" APPEND PROPERTY COMPILE_DEFINITIONS LYFLOW_GAP_${_gap_cap}=1)
    endif()
  endforeach()
endforeach()
if(TARGET lyflow_gap_compat)
  list(APPEND GAP_OP_SOURCES
    "${LYFLOW_PACK_DIR}/ops/measure_reference.cpp"
    "${LYFLOW_PACK_DIR}/ops/legacy_model_cache.cpp")
  list(APPEND _gap_links lyflow_gap_compat)
  list(APPEND _gap_defines LYFLOW_GAP_COMPAT=1)
endif()

# 随包的片段（m8-plan L14）：snippets/*.lyflow-snippet.json 编进包里，经 manifest 的 snippets 段
# 给出（ops/snippets.cpp 注册）。按字节写成十六进制数组：不受 MSVC 字符串字面量长度的限制，
# 也不经过源文件编码。内容一改就重新 configure。
file(GLOB GAP_SNIPPETS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/snippets/*.lyflow-snippet.json")
list(SORT GAP_SNIPPETS)
set(GAP_GENERATED "${CMAKE_BINARY_DIR}/generated/gap")
set(_gap_inc "// 由 solutions/gap/lyflow_op_pack.cmake 从 snippets/*.lyflow-snippet.json 生成，勿手改。\n")
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
  NAME solution-gap
  VERSION 0.1.0
  DEFAULT OFF
  SOURCES ${GAP_OP_SOURCES}
  TEST_SOURCES ${GAP_TESTS}
  PCH "${LYFLOW_PACK_DIR}/ops/gap_pch.h"
  INCLUDES "${LYFLOW_PACK_DIR}" "${LYFLOW_PACK_DIR}/algo" "${GAP_GENERATED}"
  LINK ${_gap_links}
  DEFINES ${_gap_defines}
  OPTIONS /wd4275
)

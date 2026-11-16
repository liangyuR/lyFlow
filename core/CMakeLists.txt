cmake_minimum_required(VERSION 3.20)

# vcpkg 工具链必须在 project() 之前设好，否则算子包里的 find_package 找不到 PCL。
# VCPKG_ROOT 没设时按 C:/vcpkg 兜底 —— 这是本项目的约定安装位置。
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VCPKG_ROOT})
    set(LYFLOW_VCPKG_ROOT "$ENV{VCPKG_ROOT}")
  else()
    set(LYFLOW_VCPKG_ROOT "C:/vcpkg")
  endif()
  if(EXISTS "${LYFLOW_VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${LYFLOW_VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain")
  endif()
endif()

project(lyflow_core LANGUAGES CXX VERSION 1.4.5)
option(LYFLOW_RELEASE_BUILD "Release dependency inputs were verified by package-windows.ps1" OFF)
option(BUILD_TESTING "Build lyFlow tests and client fixtures" ON)
option(LYFLOW_PACK_TESTS_ONLY "Build only enabled operator-pack tests" OFF)
option(LYFLOW_P1_MODEL_TESTS "Test the fixed P1 detector with explicitly prepared model assets" OFF)
option(LYFLOW_P4_MODEL_TESTS "Test fixed P4 segmentation and OCR models with prepared public assets" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# D8：core 固定按 RelWithDebInfo 编，不跟 cargo profile 联动。
# Rust 永远用 /MD，而 vcpkg 的 debug 库是 /MDd —— 混用会在完全无关的地方崩。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# 所有产物（DLL、exe、vcpkg applocal 拷来的依赖 DLL）都落在同一个 bin/，
# bridge/build.rs 整目录拷走它（见 README「构建」）。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(MSVC)
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
  set(CMAKE_INSTALL_OPENMP_LIBRARIES TRUE)
  include(InstallRequiredSystemLibraries)
  if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    message(FATAL_ERROR "Visual C++ runtime redistributables not found")
  endif()
  file(COPY ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

# ADR-0014：core 零第三方依赖。PCL 的 find_package 归 packs/std-pointcloud/。
set(LYFLOW_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(LYFLOW_SRC     "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(LYFLOW_TP      "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(LYFLOW_TESTS   "${CMAKE_CURRENT_SOURCE_DIR}/tests")
set(LYFLOW_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/lyflow-utf8.manifest")

# D9：exe 内嵌 activeCodePage=UTF-8，否则中文路径在 ANSI 代码页下打不开。
# 只有 exe 的 manifest 才决定进程 ACP —— DLL 上贴无效（README「构建」）。
function(lyflow_apply_common target)
  target_include_directories(${target} PRIVATE "${LYFLOW_INCLUDE}" "${LYFLOW_SRC}" "${LYFLOW_TP}")
  target_compile_definitions(${target} PRIVATE LYFLOW_BUILDING_DLL)
  if(MSVC)
    # /utf-8 与 /EHsc 都必须显式写，少一个的后果分别是「中文变乱码」和
    # 「try/catch 静默失效」——两条的完整来龙去脉见 README「构建」。
    target_compile_options(${target} PRIVATE /EHsc /W4 /permissive- /utf-8 /bigobj)
    # 第三方头（PCL/Eigen/领域库）在 /W4 下噪声极大，且不是我们能修的
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/module-graph.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/kernel-modules.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/backend-modules.cmake")

# ------------------------------------------------------------------- 算子包
# 两类包、两个开关、空串为何按「没设」处理，全在 docs/op-packs.md（ADR-0013 / ADR-0014）。
if(NOT DEFINED LYFLOW_STD_PACKS OR "${LYFLOW_STD_PACKS}" STREQUAL "")
  if(DEFINED ENV{LYFLOW_STD_PACKS} AND NOT "$ENV{LYFLOW_STD_PACKS}" STREQUAL "")
    set(LYFLOW_STD_PACKS "$ENV{LYFLOW_STD_PACKS}")
  else()
    set(LYFLOW_STD_PACKS ON)
  endif()
endif()

if(NOT DEFINED LYFLOW_OP_PACKS)
  set(LYFLOW_OP_PACKS "$ENV{LYFLOW_OP_PACKS}")
endif()

# 仓库内默认关闭的包（gap 这种领域包）用它按名字打开：分号分隔的包名（ADR-0015）。
if(NOT DEFINED LYFLOW_PACKS OR "${LYFLOW_PACKS}" STREQUAL "")
  set(LYFLOW_PACKS "$ENV{LYFLOW_PACKS}")
endif()
if(NOT DEFINED LYFLOW_MODULES)
  set(LYFLOW_MODULES "$ENV{LYFLOW_MODULES}")
endif()

set(LYFLOW_STD_PACK_NAMES "")
set(LYFLOW_EXT_PACK_NAMES "")
set(LYFLOW_PACK_VERSIONS "")
set(LYFLOW_PACK_OBJECTS "")
set(LYFLOW_PACK_TEST_SOURCES "")
set(LYFLOW_PACK_INCLUDES "")
set(LYFLOW_PACK_LIBS "")

# 包在自己的 lyflow_op_pack.cmake 里调它。必须是 macro：函数里的 set 出不了作用域。
# 每个包一个对象库，所以每个包可以有自己的 PCH（拆包前是全局只能有一份）。
macro(lyflow_op_pack)
  set(_opts "")
  set(_one NAME VERSION PCH DEFAULT)
  set(_multi SOURCES TEST_SOURCES INCLUDES LINK OPTIONS DEFINES)
  cmake_parse_arguments(LYPACK "${_opts}" "${_one}" "${_multi}" ${ARGN})
  if(NOT LYPACK_NAME)
    message(FATAL_ERROR "lyflow_op_pack() 缺少 NAME")
  endif()
  if(NOT LYPACK_SOURCES)
    message(FATAL_ERROR "算子包 ${LYPACK_NAME} 没有 SOURCES")
  endif()
  get_property(_registered GLOBAL PROPERTY LYFLOW_REGISTERED_PACKS)
  if(LYPACK_NAME IN_LIST _registered)
    message(FATAL_ERROR "Duplicate lyFlow pack registration: ${LYPACK_NAME}")
  endif()
  get_property(_declared GLOBAL PROPERTY LYFLOW_DECLARED_PACKS)
  if(LYPACK_NAME IN_LIST _declared)
    get_property(_default GLOBAL PROPERTY "LYFLOW_PACK_${LYPACK_NAME}_DEFAULT")
    if(DEFINED LYPACK_DEFAULT AND ((LYPACK_DEFAULT AND NOT _default) OR (_default AND NOT LYPACK_DEFAULT)))
      message(FATAL_ERROR "Pack ${LYPACK_NAME} DEFAULT differs from its dependency declaration")
    endif()
    set(LYPACK_DEFAULT "${_default}")
    get_property(_modules GLOBAL PROPERTY "LYFLOW_PACK_${LYPACK_NAME}_ACTIVE_MODULES")
    lyflow_resolve_modules(_closure ${_modules})
    set(_allowed "")
    foreach(_module IN LISTS _closure)
      get_property(_target GLOBAL PROPERTY "LYFLOW_MODULE_${_module}_TARGET")
      list(APPEND _allowed "${_target}")
    endforeach()
    foreach(_link IN LISTS LYPACK_LINK)
      if(_link MATCHES "^lyflow_" AND NOT _link IN_LIST _allowed)
        message(FATAL_ERROR "Pack ${LYPACK_NAME} links undeclared module target ${_link}")
      endif()
    endforeach()
  endif()
  set_property(GLOBAL APPEND PROPERTY LYFLOW_REGISTERED_PACKS "${LYPACK_NAME}")

  # DEFAULT OFF 的仓库内包要在 LYFLOW_PACKS 里点名才编（T8）。外部包是显式列出的，
  # 不受它管。包在调用之后读 LYFLOW_PACK_ENABLED 决定要不要做依赖检查与拷 DLL。
  set(LYFLOW_PACK_ENABLED TRUE)
  if(LYFLOW_PACK_IS_STD AND DEFINED LYPACK_DEFAULT AND NOT LYPACK_DEFAULT)
    set(LYFLOW_PACK_ENABLED FALSE)
    if("${LYPACK_NAME}" IN_LIST LYFLOW_PACKS)
      set(LYFLOW_PACK_ENABLED TRUE)
    endif()
  endif()
  if(NOT LYFLOW_PACK_ENABLED)
    message(STATUS "lyflow: 跳过默认关闭的包 ${LYPACK_NAME}（LYFLOW_PACKS=${LYPACK_NAME} 打开）")
  else()

  # 包名进 C++ 命名空间，所以 std-pointcloud 这种带连字符的要先转成标识符
  string(MAKE_C_IDENTIFIER "${LYPACK_NAME}" LYPACK_IDENT)
  set(LYPACK_TARGET "lyflow_pack_${LYPACK_IDENT}")

  add_library(${LYPACK_TARGET} OBJECT ${LYPACK_SOURCES})
  lyflow_apply_common(${LYPACK_TARGET})
  if(LYPACK_INCLUDES)
    target_include_directories(${LYPACK_TARGET} PRIVATE ${LYPACK_INCLUDES})
  endif()
  if(LYPACK_LINK)
    # OBJECT 库上的 link 只为拿到 include 目录与编译选项；真正的链接在 lyflow_core
    target_link_libraries(${LYPACK_TARGET} PUBLIC ${LYPACK_LINK})
  endif()
  if(LYPACK_DEFINES)
    target_compile_definitions(${LYPACK_TARGET} PRIVATE ${LYPACK_DEFINES})
  endif()
  if(MSVC)
    # 包里 include 的是别人家的头（PCL/Eigen/领域库），/W4 的噪声不是包作者能修的
    target_compile_options(${LYPACK_TARGET} PRIVATE
      /wd4127 /wd4267 /wd4244 /wd4324 /wd4100 /wd4459)
  endif()
  if(LYPACK_OPTIONS)
    target_compile_options(${LYPACK_TARGET} PRIVATE ${LYPACK_OPTIONS})
  endif()
  # 包的每个 TU 都要拖一遍 PCL + Eigen + 领域头，不预编译的话一次增量重建要十秒，
  # 热重载就跟不上手了。
  if(LYPACK_PCH)
    target_precompile_headers(${LYPACK_TARGET} PRIVATE "${LYPACK_PCH}")
  else()
    target_precompile_headers(${LYPACK_TARGET} PRIVATE <nlohmann/json.hpp>)
  endif()

  if(LYFLOW_PACK_IS_STD)
    list(APPEND LYFLOW_STD_PACK_NAMES ${LYPACK_NAME})
  else()
    list(APPEND LYFLOW_EXT_PACK_NAMES ${LYPACK_NAME})
  endif()
  list(APPEND LYFLOW_PACK_VERSIONS "${LYPACK_VERSION}")
  set_property(GLOBAL PROPERTY "LYFLOW_REGISTERED_PACK_${LYPACK_NAME}_VERSION" "${LYPACK_VERSION}")
  list(APPEND LYFLOW_PACK_OBJECTS "$<TARGET_OBJECTS:${LYPACK_TARGET}>")
  list(APPEND LYFLOW_PACK_TEST_SOURCES ${LYPACK_TEST_SOURCES})
  list(APPEND LYFLOW_PACK_INCLUDES ${LYPACK_INCLUDES})
  list(APPEND LYFLOW_PACK_LIBS ${LYPACK_LINK})
  endif()
endmacro()

set(LYFLOW_STD_PACK_DIRS "")
set(LYFLOW_EXT_PACK_DIRS "")
set(_std_pack_names "")
set(_selected_packs "")
set(_legacy_external "")
file(GLOB _std_candidates "${CMAKE_CURRENT_SOURCE_DIR}/../packs/*" "${CMAKE_CURRENT_SOURCE_DIR}/../solutions/*")
list(SORT _std_candidates)
foreach(_directory IN LISTS _std_candidates)
  get_filename_component(_directory "${_directory}" ABSOLUTE)
  if(NOT EXISTS "${_directory}/lyflow_op_pack.cmake")
    continue()
  endif()
  get_filename_component(_name "${_directory}" NAME)
  if(NOT EXISTS "${_directory}/lyflow_pack_dependencies.cmake")
    message(FATAL_ERROR "Pack ${_name} is missing lyflow_pack_dependencies.cmake")
  endif()
  get_property(_before GLOBAL PROPERTY LYFLOW_DECLARED_PACKS)
  include("${_directory}/lyflow_pack_dependencies.cmake")
  get_property(_declared GLOBAL PROPERTY LYFLOW_DECLARED_PACKS)
  set(_new "${_declared}")
  list(REMOVE_ITEM _new ${_before})
  list(LENGTH _new _count)
  if(NOT _count EQUAL 1)
    message(FATAL_ERROR "Pack directory ${_directory} must declare exactly one dependency identity")
  endif()
  list(GET _new 0 _name)
  set_property(GLOBAL PROPERTY "LYFLOW_PACK_${_name}_DIRECTORY" "${_directory}")
  set_property(GLOBAL PROPERTY "LYFLOW_PACK_${_name}_STANDARD" TRUE)
  get_property(_default GLOBAL PROPERTY "LYFLOW_PACK_${_name}_DEFAULT")
  list(APPEND _std_pack_names "${_name}")
  if((LYFLOW_STD_PACKS AND _default) OR _name IN_LIST LYFLOW_PACKS)
    list(APPEND _selected_packs "${_name}")
    list(APPEND LYFLOW_STD_PACK_DIRS "${_directory}")
  endif()
endforeach()
foreach(_want IN LISTS LYFLOW_PACKS)
  if(NOT _want IN_LIST _std_pack_names)
    get_property(_replacement GLOBAL PROPERTY "LYFLOW_REPLACED_PACK_${_want}")
    if(_replacement)
      message(FATAL_ERROR "Pack ${_want} has migrated to ${_replacement}. Select the new pack and its declared PACKS dependencies explicitly; no compatibility alias is enabled.")
    endif()
    message(FATAL_ERROR "Unknown pack in LYFLOW_PACKS: ${_want}")
  endif()
endforeach()
foreach(_raw_pack IN LISTS LYFLOW_OP_PACKS)
  if(_raw_pack STREQUAL "")
    continue()
  endif()
  file(TO_CMAKE_PATH "${_raw_pack}" _directory)
  get_filename_component(_directory "${_directory}" ABSOLUTE)
  if(NOT EXISTS "${_directory}/lyflow_op_pack.cmake")
    message(FATAL_ERROR "算子包目录 ${_directory} 里没有 lyflow_op_pack.cmake")
  endif()
  list(APPEND LYFLOW_EXT_PACK_DIRS "${_directory}")
  if(EXISTS "${_directory}/lyflow_pack_dependencies.cmake")
    get_property(_before GLOBAL PROPERTY LYFLOW_DECLARED_PACKS)
    include("${_directory}/lyflow_pack_dependencies.cmake")
    get_property(_after GLOBAL PROPERTY LYFLOW_DECLARED_PACKS)
    list(REMOVE_ITEM _after ${_before})
    list(LENGTH _after _count)
    if(NOT _count EQUAL 1)
      message(FATAL_ERROR "External pack ${_directory} must declare exactly one pack identity")
    endif()
    list(GET _after 0 _name)
    set_property(GLOBAL PROPERTY "LYFLOW_PACK_${_name}_DIRECTORY" "${_directory}")
    set_property(GLOBAL PROPERTY "LYFLOW_PACK_${_name}_STANDARD" FALSE)
    list(APPEND _selected_packs "${_name}")
  else()
    list(APPEND _legacy_external "${_directory}")
  endif()
endforeach()
lyflow_resolve_packs(_ordered_packs ${_selected_packs})
lyflow_resolve_pack_modules(_requested_modules PACKS ${_ordered_packs} MODULES ${LYFLOW_MODULES})
lyflow_configure_modules(${_requested_modules})
foreach(_pack IN LISTS _ordered_packs)
  get_property(LYFLOW_PACK_DIR GLOBAL PROPERTY "LYFLOW_PACK_${_pack}_DIRECTORY")
  get_property(LYFLOW_PACK_IS_STD GLOBAL PROPERTY "LYFLOW_PACK_${_pack}_STANDARD")
  set(LYFLOW_PACK_EXPECTED_NAME "${_pack}")
  include("${LYFLOW_PACK_DIR}/lyflow_op_pack.cmake")
  if(NOT LYPACK_NAME STREQUAL LYFLOW_PACK_EXPECTED_NAME)
    message(FATAL_ERROR "Pack registration ${LYPACK_NAME} differs from declared identity ${LYFLOW_PACK_EXPECTED_NAME}")
  endif()
endforeach()
foreach(LYFLOW_PACK_DIR IN LISTS _legacy_external)
  set(LYFLOW_PACK_IS_STD FALSE)
  include("${LYFLOW_PACK_DIR}/lyflow_op_pack.cmake")
endforeach()
get_property(LYFLOW_ENABLED_MODULES GLOBAL PROPERTY LYFLOW_ENABLED_MODULES)
message(STATUS "lyflow: modules = ${LYFLOW_ENABLED_MODULES}")
message(STATUS "lyflow: std packs = ${LYFLOW_STD_PACK_NAMES}")
message(STATUS "lyflow: external op packs = ${LYFLOW_EXT_PACK_NAMES}")

# 注册入口是生成的：每个包出一个 lyflow::packs::<name>::registerPackOps。
# 手写一张表的话，加一个包要同时改 core 和包两边。
set(LYFLOW_PACK_DECLS "")
set(LYFLOW_STD_CALLS "")
set(LYFLOW_EXT_CALLS "")
foreach(name IN LISTS LYFLOW_STD_PACK_NAMES LYFLOW_EXT_PACK_NAMES)
  string(MAKE_C_IDENTIFIER "${name}" ident)
  get_property(version GLOBAL PROPERTY "LYFLOW_REGISTERED_PACK_${name}_VERSION")
  set(tag "${name}")
  if(version)
    set(tag "${name}@${version}")
  endif()
  string(APPEND LYFLOW_PACK_DECLS
         "namespace lyflow::packs::${ident} { void registerPackOps(::lyflow::Registry&); }\n")
  set(call "  r.setCurrentPack(\"${tag}\");\n  packs::${ident}::registerPackOps(r);\n  r.setCurrentPack({});\n")
  if("${name}" IN_LIST LYFLOW_STD_PACK_NAMES)
    string(APPEND LYFLOW_STD_CALLS "${call}")
  else()
    string(APPEND LYFLOW_EXT_CALLS "${call}")
  endif()
endforeach()

set(LYFLOW_PACK_REGISTRAR "${CMAKE_BINARY_DIR}/generated/op_packs.cpp")
# 先写 .in 再 COPYONLY：内容没变时 configure_file 不动时间戳，
# 否则每次 configure 都要重编这个 TU 并重链 DLL。
file(WRITE "${LYFLOW_PACK_REGISTRAR}.in"
"// 由 core/CMakeLists.txt 生成，勿手改。算子包机制见 docs/op-packs.md。
#include \"lyflow/registry.h\"

${LYFLOW_PACK_DECLS}
namespace lyflow {

void registerStdPacks(Registry& r) {
  (void)r;
${LYFLOW_STD_CALLS}}

void registerExternalPacks(Registry& r) {
  (void)r;
${LYFLOW_EXT_CALLS}}

}  // namespace lyflow
")
configure_file("${LYFLOW_PACK_REGISTRAR}.in" "${LYFLOW_PACK_REGISTRAR}" COPYONLY)

# ------------------------------------------------------------------- 目标
file(GLOB LYFLOW_CORE_SOURCES CONFIGURE_DEPENDS
  "${LYFLOW_SRC}/*.cpp" "${LYFLOW_SRC}/exec/*.cpp" "${LYFLOW_SRC}/ops/*.cpp")

# 编辑器 e2e 用的测试算子（param-recipe P2.10）：编进 core，只在 LYFLOW_TEST_OPS=1 时注册。
# 在 tests/ 的子目录里，不会被下面 doctest 的 tests/*.cpp 通配再编一份。
set(LYFLOW_E2E_OPS "${LYFLOW_TESTS}/e2e/register_e2e_ops.cpp")

add_library(lyflow_core_obj OBJECT ${LYFLOW_CORE_SOURCES} "${LYFLOW_PACK_REGISTRAR}" "${LYFLOW_E2E_OPS}")
lyflow_apply_common(lyflow_core_obj)
target_precompile_headers(lyflow_core_obj PRIVATE <nlohmann/json.hpp>)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/release-identity.cmake")
set(_release_configuration "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/release.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_release_configuration}")
lyflow_release_combination_matches(_release_combination_matches "${_release_configuration}"
  "${LYFLOW_STD_PACK_NAMES}" "${LYFLOW_MODULES}")
set(LYFLOW_RELEASE_IDENTITY_ALLOWED FALSE)
if(LYFLOW_RELEASE_BUILD AND WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
   CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" AND NOT LYFLOW_EXT_PACK_DIRS AND
   NOT LYFLOW_EXT_PACK_NAMES AND _release_combination_matches)
  set(LYFLOW_RELEASE_IDENTITY_ALLOWED TRUE)
endif()
add_custom_target(lyflow-source-revision
  COMMAND "${CMAKE_COMMAND}" "-DLYFLOW_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/.."
          "-DLYFLOW_GENERATED_DIR=${CMAKE_BINARY_DIR}/generated"
          "-DLYFLOW_RELEASE_IDENTITY_ALLOWED=${LYFLOW_RELEASE_IDENTITY_ALLOWED}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/update-source-revision.cmake"
  BYPRODUCTS "${CMAKE_BINARY_DIR}/generated/source_revision.h"
  VERBATIM)
add_dependencies(lyflow_core_obj lyflow-source-revision)
target_include_directories(lyflow_core_obj PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_include_directories(lyflow_core_obj PRIVATE "${LYFLOW_TESTS}")

# D1：core 是 DLL，只导出 C ABI。Rust 用 libloading 运行时加载。
add_library(lyflow_core SHARED
  $<TARGET_OBJECTS:lyflow_core_obj> ${LYFLOW_PACK_OBJECTS})
target_include_directories(lyflow_core PUBLIC "${LYFLOW_INCLUDE}")
target_link_libraries(lyflow_core PRIVATE ${LYFLOW_PACK_LIBS})
set_target_properties(lyflow_core PROPERTIES
  # Rust 侧按固定名字 LoadLibrary，不要带 lib 前缀或版本后缀
  OUTPUT_NAME "lyflow_core")
if(UNIX AND NOT APPLE)
  set_target_properties(lyflow_core PROPERTIES
    BUILD_RPATH_USE_ORIGIN TRUE
    INSTALL_RPATH "$ORIGIN")
endif()

# 调试工具与测试。都直接吃对象库而不是链 DLL —— DLL 只导出 C ABI，
# 而这两个要用的是 Registry / Executor 这些 C++ 内部类。
add_executable(lyflow-dump-manifest
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/dump_manifest.cpp"
  "${LYFLOW_MANIFEST}"
  $<TARGET_OBJECTS:lyflow_core_obj> ${LYFLOW_PACK_OBJECTS})
lyflow_apply_common(lyflow-dump-manifest)
target_link_libraries(lyflow-dump-manifest PRIVATE ${LYFLOW_PACK_LIBS})

# 带包构建时把包自己的测试源也编进同一个 doctest 目标（不另起一个 exe）。
if(BUILD_TESTING)
  file(GLOB LYFLOW_TEST_SOURCES CONFIGURE_DEPENDS "${LYFLOW_TESTS}/*.cpp")
  if(LYFLOW_PACK_TESTS_ONLY)
    if(NOT LYFLOW_PACK_TEST_SOURCES)
      message(FATAL_ERROR "LYFLOW_PACK_TESTS_ONLY requires enabled operator-pack tests")
    endif()
    set(LYFLOW_TEST_SOURCES "${LYFLOW_TESTS}/doctest_main.cpp")
  endif()
  if("std-value" IN_LIST LYFLOW_STD_PACK_NAMES AND
     "std-geometry" IN_LIST LYFLOW_STD_PACK_NAMES AND
     "std-image" IN_LIST LYFLOW_STD_PACK_NAMES)
    list(APPEND LYFLOW_TEST_SOURCES "${LYFLOW_TESTS}/integration/p0_pipeline.cpp")
    list(APPEND LYFLOW_TEST_SOURCES "${LYFLOW_TESTS}/integration/p2_highlights.cpp")
    list(APPEND LYFLOW_TEST_SOURCES "${LYFLOW_TESTS}/integration/p3_calibration.cpp")
    list(APPEND LYFLOW_TEST_SOURCES "${LYFLOW_TESTS}/integration/p4_recognition.cpp")
    list(APPEND LYFLOW_TEST_SOURCES "${LYFLOW_TESTS}/integration/p5_preview.cpp")
  endif()
  if("std-value" IN_LIST LYFLOW_STD_PACK_NAMES)
    list(APPEND LYFLOW_TEST_SOURCES "${LYFLOW_TESTS}/integration/p2_expression.cpp")
  endif()
  if("std-geometry" IN_LIST LYFLOW_STD_PACK_NAMES AND "std-pointcloud" IN_LIST LYFLOW_STD_PACK_NAMES)
    list(APPEND LYFLOW_TEST_SOURCES "${LYFLOW_TESTS}/integration/test_constraints_fit_pipeline.cpp")
    list(APPEND LYFLOW_TEST_SOURCES "${LYFLOW_TESTS}/integration/test_pointcloud_geometry_pipeline.cpp")
  endif()
  if("std-value" IN_LIST LYFLOW_STD_PACK_NAMES AND
     "std-geometry" IN_LIST LYFLOW_STD_PACK_NAMES AND
     "std-pointcloud" IN_LIST LYFLOW_STD_PACK_NAMES AND
     "std-image" IN_LIST LYFLOW_STD_PACK_NAMES)
    list(APPEND LYFLOW_TEST_SOURCES "${LYFLOW_TESTS}/integration/p2_cloud_rules.cpp")
    list(APPEND LYFLOW_TEST_SOURCES "${LYFLOW_TESTS}/integration/p3_shape.cpp")
    list(APPEND LYFLOW_TEST_SOURCES "${LYFLOW_TESTS}/integration/p5_depth_geometry.cpp")
  endif()
  if(LYFLOW_P1_MODEL_TESTS)
    if(NOT "std-image" IN_LIST LYFLOW_STD_PACK_NAMES OR NOT "std-ml" IN_LIST LYFLOW_STD_PACK_NAMES)
      message(FATAL_ERROR "LYFLOW_P1_MODEL_TESTS requires std-image and std-ml")
    endif()
    list(APPEND LYFLOW_TEST_SOURCES "${LYFLOW_TESTS}/integration/p1_detection.cpp")
  endif()
  add_executable(lyflow-core-tests
    ${LYFLOW_TEST_SOURCES} ${LYFLOW_PACK_TEST_SOURCES}
    "${LYFLOW_MANIFEST}"
    $<TARGET_OBJECTS:lyflow_core_obj> ${LYFLOW_PACK_OBJECTS})
  lyflow_apply_common(lyflow-core-tests)
  target_precompile_headers(lyflow-core-tests PRIVATE <doctest/doctest.h> <nlohmann/json.hpp>)
  set_source_files_properties("${LYFLOW_TESTS}/doctest_main.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  # 包的测试也 #include "helpers.h"，而它在 core/tests/ 下
  target_include_directories(lyflow-core-tests PRIVATE "${LYFLOW_TESTS}")
  target_compile_definitions(lyflow-core-tests PRIVATE
    "LYFLOW_PYTHON_SDK_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../python\""
    "LYFLOW_EXAMPLES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../examples\""
    "LYFLOW_TEST_RUNTIME=\"$<TARGET_FILE:lyflow_core>\"")
  add_dependencies(lyflow-core-tests lyflow_core)
  if(LYFLOW_PACK_INCLUDES)
    target_include_directories(lyflow-core-tests PRIVATE ${LYFLOW_PACK_INCLUDES})
  endif()
  if(MSVC)
    # doctest 为了 stringify 特化了 std::tuple，C++20 起标准禁止，MSVC 会警告。
    # 不是我们能修的第三方代码，单独关掉这一条，其余 /W4 照旧。
    target_compile_options(lyflow-core-tests PRIVATE /wd5285)
  endif()
  target_link_libraries(lyflow-core-tests PRIVATE ${LYFLOW_PACK_LIBS})
  if("std-image" IN_LIST LYFLOW_STD_PACK_NAMES)
    lyflow_add_barcode_contract_tests(lyflow-core-tests)
  endif()
  if(LYFLOW_P4_MODEL_TESTS)
    if(NOT "std-image" IN_LIST LYFLOW_STD_PACK_NAMES OR NOT "std-ml" IN_LIST LYFLOW_STD_PACK_NAMES)
      message(FATAL_ERROR "LYFLOW_P4_MODEL_TESTS requires std-image and std-ml")
    endif()
    target_compile_definitions(lyflow-core-tests PRIVATE LYFLOW_P4_MODEL_TESTS=1)
  endif()

  math(EXPR _fixture_next_patch "${PROJECT_VERSION_PATCH} + 1")
  set(_fixture_other_version "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${_fixture_next_patch}")
  math(EXPR _fixture_next_minor "${PROJECT_VERSION_MINOR} + 1")
  set(_fixture_other_minor "${PROJECT_VERSION_MAJOR}.${_fixture_next_minor}.0")
  foreach(fixture matching legacy abi_patch engine_patch abi_minor engine_minor null_abi)
    add_library(lyflow-client-${fixture} SHARED "${LYFLOW_TESTS}/fixtures/client_version.cpp")
    if(WIN32 AND fixture STREQUAL "matching")
      target_link_libraries(lyflow-client-${fixture} PRIVATE winmm)
    endif()
    target_include_directories(lyflow-client-${fixture} PRIVATE "${LYFLOW_INCLUDE}")
    # version.h 里有中文注释：中文代码页（936）下不加 /utf-8，注释会吞掉下一行的 #define
    target_compile_options(lyflow-client-${fixture} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_compile_definitions(lyflow-client-${fixture} PRIVATE "LYFLOW_FIXTURE_${fixture}"
      "LYFLOW_FIXTURE_OTHER_VERSION=\"${_fixture_other_version}\""
      "LYFLOW_FIXTURE_OTHER_MINOR=\"${_fixture_other_minor}\"")
    set_target_properties(lyflow-client-${fixture} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client-fixtures"
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/client-fixtures"
      MSVC_RUNTIME_LIBRARY "MultiThreaded")
    add_dependencies(lyflow-core-tests lyflow-client-${fixture})
    target_compile_definitions(lyflow-core-tests PRIVATE
      "LYFLOW_FIXTURE_PATH_${fixture}=\"$<TARGET_FILE:lyflow-client-${fixture}>\"")
  endforeach()

  enable_testing()
  add_test(NAME lyflow-core-tests COMMAND lyflow-core-tests)
  set(_kernel_tests "")
  set(_kernel_libraries "")
  if(TARGET lyflow_geometry_kernel)
    list(APPEND _kernel_tests "${LYFLOW_KERNEL_ROOT}/tests/test_geometry.cpp" "${LYFLOW_KERNEL_ROOT}/tests/test_constraints.cpp")
    list(APPEND _kernel_libraries lyflow_geometry_kernel)
  endif()
  if(TARGET lyflow_metrology_kernel)
    list(APPEND _kernel_tests "${LYFLOW_KERNEL_ROOT}/tests/test_metrology.cpp"
      "${LYFLOW_KERNEL_ROOT}/tests/test_profile_dimensions.cpp")
    list(APPEND _kernel_libraries lyflow_metrology_kernel)
  endif()
  if(TARGET lyflow_profile_kernel)
    list(APPEND _kernel_tests "${LYFLOW_KERNEL_ROOT}/tests/test_profile.cpp")
    list(APPEND _kernel_libraries lyflow_profile_kernel)
  endif()
  if(TARGET lyflow_profile_analysis_kernel)
    list(APPEND _kernel_tests "${LYFLOW_KERNEL_ROOT}/tests/test_profile_analysis.cpp")
    list(APPEND _kernel_libraries lyflow_profile_analysis_kernel)
  endif()
  if(TARGET lyflow_profile_robust_kernel)
    list(APPEND _kernel_tests "${LYFLOW_KERNEL_ROOT}/tests/test_profile_robust.cpp"
      "${LYFLOW_KERNEL_ROOT}/tests/test_profile_features.cpp")
    list(APPEND _kernel_libraries lyflow_profile_robust_kernel)
  endif()
  if(_kernel_tests)
    add_executable(lyflow-kernel-tests "${LYFLOW_TESTS}/doctest_main.cpp" ${_kernel_tests})
    target_link_libraries(lyflow-kernel-tests PRIVATE ${_kernel_libraries})
    if(MSVC)
      target_compile_options(lyflow-kernel-tests PRIVATE /utf-8 /EHsc /wd5285)
    endif()
    add_test(NAME lyflow-kernel-tests COMMAND lyflow-kernel-tests)
  endif()
endif()

# --------------------------------------------------------------- 安装布局
# A1-11 / docs/embedding.md。产出 bin（core DLL + 依赖 DLL + lyflow CLI）、
# include/lyflow、空的 library/、examples/ 与 lyflow-config.cmake。
# CLI 由 cargo 构建，所以它是 scripts/install-lyflow.ps1 拷进去的，不在这里。
include(CMakePackageConfigHelpers)

set(LYFLOW_INSTALL_VERSION "${PROJECT_VERSION}")
set(LYFLOW_INSTALL_ABI_VERSION "${PROJECT_VERSION}")
set(LYFLOW_INSTALL_DLL_NAME "$<TARGET_FILE_NAME:lyflow_core>")
set(LYFLOW_INSTALL_PROFILE_PCL_VERSION "")
if(TARGET lyflow_profile_robust_kernel)
  get_target_property(LYFLOW_INSTALL_PROFILE_PCL_VERSION lyflow_profile_robust_kernel LYFLOW_PROFILE_PCL_VERSION)
  if(NOT LYFLOW_INSTALL_PROFILE_PCL_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+([.][0-9]+)?$")
    message(FATAL_ERROR "The public profile robust kernel must declare its build PCL version")
  endif()
endif()

execute_process(
  COMMAND git rev-parse HEAD
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
  OUTPUT_VARIABLE LYFLOW_INSTALL_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT LYFLOW_INSTALL_COMMIT)
  set(LYFLOW_INSTALL_COMMIT "unknown")
endif()
execute_process(
  COMMAND git status --porcelain
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
  OUTPUT_VARIABLE LYFLOW_INSTALL_DIRTY
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(LYFLOW_INSTALL_DIRTY)
  set(LYFLOW_INSTALL_COMMIT "${LYFLOW_INSTALL_COMMIT}-dirty")
endif()

install(TARGETS lyflow_core
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION bin
        ARCHIVE DESTINATION lib)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../python/lyflow_python"
        DESTINATION bin/python
        FILES_MATCHING PATTERN "*.py")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../python/examples/"
        DESTINATION examples/python
        FILES_MATCHING PATTERN "*.py" PATTERN "*.md")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../python/examples/"
        DESTINATION bin/python/examples
        FILES_MATCHING PATTERN "*.py" PATTERN "*.md")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../python/README.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/../python/pyproject.toml"
        DESTINATION bin/python)

# vcpkg 的 applocal 部署把 PCL/boost/onnxruntime 那些依赖 DLL 拷进了 build 的 bin/。
# 整目录带走 —— 少一个的表现是宿主 LoadLibrary 失败而错误信息只说「找不到模块」。
install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/"
        DESTINATION bin
        FILES_MATCHING PATTERN "*.dll")

install(DIRECTORY "${LYFLOW_INCLUDE}/lyflow"
        DESTINATION include
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
if(TARGET lyflow_data_contract)
  install(DIRECTORY "${LYFLOW_KERNEL_ROOT}/include/lyflow/kernel" DESTINATION include/lyflow)
  install(EXPORT lyflow-kernel-targets NAMESPACE lyflow:: DESTINATION lib/cmake/lyflow)
endif()
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/json.hpp"
        DESTINATION include/nlohmann)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/LICENSE.MIT"
        DESTINATION licenses/core/nlohmann)
install(FILES "${LYFLOW_TP}/xxhash/xxhash.h" DESTINATION include/xxhash)
install(FILES "${LYFLOW_TP}/xxhash/LICENSE" DESTINATION licenses/core/xxhash)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/lyflow-utf8.manifest"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/embed_minimal.cpp"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/embed_concurrent.cpp"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/embed_inject.cpp"
        DESTINATION examples)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p0-inspection.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p0-stripe.pgm"
        DESTINATION examples)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p2-highlights.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p2-highlights.pgm"
        DESTINATION examples)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p2-expression.lyflow.json"
        DESTINATION examples)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p2-height-rules.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p2-height-rules.pcd"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p2-depth-height.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p2-depth-height.pgm"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p2-constrained-fit.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p2-matching-fast.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p2-matching-balanced.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p2-matching-precise.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p2-model-cad.pcd"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p2-scene.pcd"
        DESTINATION examples)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-marker-workspace.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-stag-workspace.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-workspace-native.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-workspace-legacy.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-shape-cad.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-shape-parallel.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-marker-scene.png"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-stag-hd15.png"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-shape-L-scene.png"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-shape-parallel-scene.png"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-camera.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-camera-stag.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-workspace.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-workspace-legacy.yaml"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-cad-L.stl"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-cad-H.stl"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-fixtures.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-assets.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p3-asset-licenses.txt"
              "${CMAKE_CURRENT_SOURCE_DIR}/../docs/p3-examples.md"
        DESTINATION examples)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p4-annotations-native.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p4-annotations-legacy.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p4-annotations-index.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p4-annotations-native.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p4-annotations-xyz.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p4-annotations-samples_v2.txt"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/1700000000000-工件,样例.png"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p4-segmentation.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p4-segmentation-retained.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p4-ocr.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p4-ocr-scene.png"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p4-ocr-scene-truth.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p4-barcodes.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p4-qr-refine.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p4-barcodes.png"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p4-qr-source.png"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p4-barcode-truth.json"
        DESTINATION examples)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/fetch-p4-model-assets.py"
              "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/generate-p4-ocr-fixture.py"
              "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/generate-p4-barcode-fixtures.py"
        DESTINATION scripts)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/p4-model-assets.json"
        DESTINATION packaging)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../docs/p4-assets.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/../docs/p4-examples.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/../docs/p4-analysis-annotations.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/../docs/p4-analysis-barcodes.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/../docs/p4-analysis-ocr.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/../docs/p4-analysis-segmentation.md"
        DESTINATION docs)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-scene.pcd"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-plane.pcd"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-orthographic.pcd"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-camera.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-circle-camera.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/1700000001000-p5-scene.png"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-annotations.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-annotations-index.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-circle-front.png"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-circle-tilted.png"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-fixtures.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-depth-inspection.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-depth-filters.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-orthographic-roi.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-box-crop.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-pose-rigid.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-pose-project-plane.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-circles-3d.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p5-mask-remove.lyflow.json"
        DESTINATION examples)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/generate-p5-fixtures.py"
        DESTINATION scripts)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../docs/p5-examples.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/../docs/p5-analysis-projection.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/../docs/p5-analysis-cloud-inspection.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/../docs/p5-analysis-pose-geometry.md"
        DESTINATION docs)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../docs/adr/0031-depth-grid-association-and-derived-observations.md"
        DESTINATION docs/adr)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p6-delivery_demo.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p6-delivery_receiver.py"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p6-delivery_host.cpp"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p6-model-viewer.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p6-viewer-cuboid.stl"
        DESTINATION examples)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../docs/p6-examples.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/../docs/p6-analysis-python.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/../docs/p6-analysis-renderer.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/../docs/p6-analysis-visualization.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/../docs/p6-analysis-delivery.md"
        DESTINATION docs)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../docs/adr/0032-native-frames-and-result-delivery.md"
        DESTINATION docs/adr)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../docs/profile-data-contract.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/../docs/profile-host-protocol.md"
        DESTINATION docs)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../examples/m1-profile-adapters.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/embed_profile.cpp"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/m3-native-profile.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/m3-native-profile.csv"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/m3-native-profile.md"
        DESTINATION examples)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p1-cloud.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p1-clusters.pcd"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p1-pose-flow.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p1-detection.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p1-yolo-detection.lyflow.json"
              "${CMAKE_CURRENT_SOURCE_DIR}/../examples/p1-detection-README.md"
        DESTINATION examples)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/fetch-p1-model-assets.py"
              "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/export-p1-yolo-models.py"
              "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/requirements-model-export.txt"
        DESTINATION scripts)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/p1-model-assets.json"
        DESTINATION packaging)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/licenses/"
        DESTINATION packaging/licenses)

# 库算子目录（ADR-0010）。宿主往里放 *.lyflow-op.json，所以布局里要有它。
install(CODE "file(MAKE_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/library\")")

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/lyflow-config.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/lyflow-config.cmake"
  INSTALL_DESTINATION ".")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/lyflow-config-version.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY ExactVersion)

# DLL 名字里有生成器表达式，configure_package_config_file 展不开，所以再过一道 file(GENERATE)。
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/lyflow-config.cmake"
     INPUT "${CMAKE_CURRENT_BINARY_DIR}/lyflow-config.cmake")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/lyflow-config.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/lyflow-config-version.cmake"
        DESTINATION ".")

if(TARGET lyflow_geometry_kernel AND TARGET lyflow_metrology_kernel AND
   TARGET lyflow_profile_kernel AND TARGET lyflow_profile_analysis_kernel)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../examples/public_kernels.cpp" DESTINATION examples)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../examples/public-kernels/CMakeLists.txt" DESTINATION examples/public-kernels)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../docs/public-kernels.md"
                "${CMAKE_CURRENT_SOURCE_DIR}/../docs/profile-public-operators.md" DESTINATION docs)
endif()

if(TARGET lyflow_profile_robust_kernel)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../examples/profile-robust/CMakeLists.txt"
                "${CMAKE_CURRENT_SOURCE_DIR}/../examples/profile-robust/main.cpp"
                "${CMAKE_CURRENT_SOURCE_DIR}/../examples/profile-robust/README.md"
          DESTINATION examples/profile-robust)
  if(TARGET lyflow_pack_std_profile AND TARGET lyflow_pack_std_geometry AND TARGET lyflow_pack_std_metrology)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../examples/m4-native-profile.lyflow.json"
                  "${CMAKE_CURRENT_SOURCE_DIR}/../examples/m4-native-profile.csv"
                  "${CMAKE_CURRENT_SOURCE_DIR}/../examples/m4-native-profile.md"
                  "${CMAKE_CURRENT_SOURCE_DIR}/../examples/embed_profile_workflow.cpp"
            DESTINATION examples)
  endif()
endif()

if(TARGET lyflow_pack_solution_dts)
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../solutions/dts/graphs/" DESTINATION examples/dts)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../solutions/dts/README.md" DESTINATION examples/dts)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../docs/dts-profile-migration.md" DESTINATION docs)
endif()

if(TARGET lyflow_pack_solution_gap)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../docs/gap-profile-migration.md" DESTINATION docs)
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../solutions/gap/README.md" DESTINATION examples/gap)
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../solutions/gap/snippets/"
          DESTINATION examples/gap/snippets FILES_MATCHING PATTERN "*.lyflow-snippet.json"
          PATTERN "model-with-template-fallback.lyflow-snippet.json" EXCLUDE)
  if(TARGET lyflow_gap_model AND TARGET lyflow_gap_template)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../solutions/gap/snippets/model-with-template-fallback.lyflow-snippet.json"
            DESTINATION examples/gap/snippets)
  endif()
endif()

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

project(lyflow_core LANGUAGES CXX VERSION 0.1.0)

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

# ------------------------------------------------------------------- 算子包
# ADR-0013 / ADR-0014。两类包：仓库内的标准包（packs/*，LYFLOW_STD_PACKS 默认 ON）
# 与外部领域包（LYFLOW_OP_PACKS，分号分隔的目录列表）。怎么写一个包见 docs/op-packs.md。
# 空串按「没设」处理：-D 传了个空值不该悄悄变成「关掉标准包」。
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
  set(_one NAME VERSION PCH)
  set(_multi SOURCES TEST_SOURCES INCLUDES LINK OPTIONS DEFINES)
  cmake_parse_arguments(LYPACK "${_opts}" "${_one}" "${_multi}" ${ARGN})
  if(NOT LYPACK_NAME)
    message(FATAL_ERROR "lyflow_op_pack() 缺少 NAME")
  endif()
  if(NOT LYPACK_SOURCES)
    message(FATAL_ERROR "算子包 ${LYPACK_NAME} 没有 SOURCES")
  endif()
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
  endif()

  if(LYFLOW_PACK_IS_STD)
    list(APPEND LYFLOW_STD_PACK_NAMES ${LYPACK_NAME})
  else()
    list(APPEND LYFLOW_EXT_PACK_NAMES ${LYPACK_NAME})
  endif()
  list(APPEND LYFLOW_PACK_VERSIONS "${LYPACK_VERSION}")
  list(APPEND LYFLOW_PACK_OBJECTS "$<TARGET_OBJECTS:${LYPACK_TARGET}>")
  list(APPEND LYFLOW_PACK_TEST_SOURCES ${LYPACK_TEST_SOURCES})
  list(APPEND LYFLOW_PACK_INCLUDES ${LYPACK_INCLUDES})
  list(APPEND LYFLOW_PACK_LIBS ${LYPACK_LINK})
endmacro()

# 收集包目录：标准包在前，外部包追加在后（S4）。
set(LYFLOW_STD_PACK_DIRS "")
if(LYFLOW_STD_PACKS)
  file(GLOB _std_candidates "${CMAKE_CURRENT_SOURCE_DIR}/../packs/*")
  list(SORT _std_candidates)
  foreach(d IN LISTS _std_candidates)
    # 路径要规范化：带 core/../ 的那一份会让 ninja 把同一个文件当成两条规则
    get_filename_component(d "${d}" ABSOLUTE)
    if(EXISTS "${d}/lyflow_op_pack.cmake")
      list(APPEND LYFLOW_STD_PACK_DIRS "${d}")
    endif()
  endforeach()
endif()

set(LYFLOW_EXT_PACK_DIRS "")
foreach(_raw_pack IN LISTS LYFLOW_OP_PACKS)
  if(_raw_pack STREQUAL "")
    continue()
  endif()
  # 环境变量里是 Windows 的反斜杠路径，CMake 会把 \p 当成非法转义。
  file(TO_CMAKE_PATH "${_raw_pack}" pack)
  list(APPEND LYFLOW_EXT_PACK_DIRS "${pack}")
endforeach()

foreach(pack IN LISTS LYFLOW_STD_PACK_DIRS LYFLOW_EXT_PACK_DIRS)
  set(LYFLOW_PACK_IS_STD FALSE)
  if("${pack}" IN_LIST LYFLOW_STD_PACK_DIRS)
    set(LYFLOW_PACK_IS_STD TRUE)
  endif()
  set(_pack_file "${pack}/lyflow_op_pack.cmake")
  if(NOT EXISTS "${_pack_file}")
    message(FATAL_ERROR "算子包目录 ${pack} 里没有 lyflow_op_pack.cmake")
  endif()
  set(LYFLOW_PACK_DIR "${pack}")
  # include() 自己会把这个文件登记成 configure 依赖；包里加 .cpp 靠包自己的
  # file(GLOB CONFIGURE_DEPENDS)。再显式登记一遍会让 ninja 报「输出定义了两次」。
  include("${_pack_file}")
endforeach()
message(STATUS "lyflow: std packs = ${LYFLOW_STD_PACK_NAMES}")
message(STATUS "lyflow: external op packs = ${LYFLOW_EXT_PACK_NAMES}")

# 注册入口是生成的：每个包出一个 lyflow::packs::<name>::registerPackOps。
# 手写一张表的话，加一个包要同时改 core 和包两边。
set(LYFLOW_PACK_DECLS "")
set(LYFLOW_STD_CALLS "")
set(LYFLOW_EXT_CALLS "")
set(_pack_index 0)
foreach(name IN LISTS LYFLOW_STD_PACK_NAMES LYFLOW_EXT_PACK_NAMES)
  string(MAKE_C_IDENTIFIER "${name}" ident)
  list(GET LYFLOW_PACK_VERSIONS ${_pack_index} version)
  math(EXPR _pack_index "${_pack_index} + 1")
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

add_library(lyflow_core_obj OBJECT ${LYFLOW_CORE_SOURCES} "${LYFLOW_PACK_REGISTRAR}")
lyflow_apply_common(lyflow_core_obj)

# D1：core 是 DLL，只导出 C ABI。Rust 用 libloading 运行时加载。
add_library(lyflow_core SHARED
  $<TARGET_OBJECTS:lyflow_core_obj> ${LYFLOW_PACK_OBJECTS})
target_include_directories(lyflow_core PUBLIC "${LYFLOW_INCLUDE}")
target_link_libraries(lyflow_core PRIVATE ${LYFLOW_PACK_LIBS})
set_target_properties(lyflow_core PROPERTIES
  # Rust 侧按固定名字 LoadLibrary，不要带 lib 前缀或版本后缀
  OUTPUT_NAME "lyflow_core")

# 调试工具与测试。都直接吃对象库而不是链 DLL —— DLL 只导出 C ABI，
# 而这两个要用的是 Registry / Executor 这些 C++ 内部类。
add_executable(lyflow-dump-manifest
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/dump_manifest.cpp"
  "${LYFLOW_MANIFEST}"
  $<TARGET_OBJECTS:lyflow_core_obj> ${LYFLOW_PACK_OBJECTS})
lyflow_apply_common(lyflow-dump-manifest)
target_link_libraries(lyflow-dump-manifest PRIVATE ${LYFLOW_PACK_LIBS})

# 带包构建时把包自己的测试源也编进同一个 doctest 目标（不另起一个 exe）。
file(GLOB LYFLOW_TEST_SOURCES CONFIGURE_DEPENDS "${LYFLOW_TESTS}/*.cpp")
add_executable(lyflow-core-tests
  ${LYFLOW_TEST_SOURCES} ${LYFLOW_PACK_TEST_SOURCES}
  "${LYFLOW_MANIFEST}"
  $<TARGET_OBJECTS:lyflow_core_obj> ${LYFLOW_PACK_OBJECTS})
lyflow_apply_common(lyflow-core-tests)
# 包的测试也 #include "helpers.h"，而它在 core/tests/ 下
target_include_directories(lyflow-core-tests PRIVATE "${LYFLOW_TESTS}")
if(LYFLOW_PACK_INCLUDES)
  target_include_directories(lyflow-core-tests PRIVATE ${LYFLOW_PACK_INCLUDES})
endif()
if(MSVC)
  # doctest 为了 stringify 特化了 std::tuple，C++20 起标准禁止，MSVC 会警告。
  # 不是我们能修的第三方代码，单独关掉这一条，其余 /W4 照旧。
  target_compile_options(lyflow-core-tests PRIVATE /wd5285)
endif()
target_link_libraries(lyflow-core-tests PRIVATE ${LYFLOW_PACK_LIBS})

enable_testing()
add_test(NAME lyflow-core-tests COMMAND lyflow-core-tests)

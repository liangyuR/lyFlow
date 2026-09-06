cmake_minimum_required(VERSION 3.20)

# vcpkg 工具链必须在 project() 之前设好，否则 find_package 找不到 PCL。
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

set(LYFLOW_PCL_COMPONENTS common io filters kdtree search segmentation sample_consensus features)
find_package(PCL 1.12 REQUIRED COMPONENTS ${LYFLOW_PCL_COMPONENTS})

# PCL::common 与 pcl_common 两种目标名都认，都没有时退回 ${PCL_LIBRARIES}。
# 为什么不能硬写一种见 README「构建」。
set(LYFLOW_PCL_LIBS "")
foreach(comp IN LISTS LYFLOW_PCL_COMPONENTS)
  if(TARGET PCL::${comp})
    list(APPEND LYFLOW_PCL_LIBS PCL::${comp})
  elseif(TARGET pcl_${comp})
    list(APPEND LYFLOW_PCL_LIBS pcl_${comp})
  endif()
endforeach()
if(NOT LYFLOW_PCL_LIBS)
  set(LYFLOW_PCL_LIBS ${PCL_LIBRARIES})
  include_directories(${PCL_INCLUDE_DIRS})
  add_definitions(${PCL_DEFINITIONS})
endif()
message(STATUS "lyflow: PCL targets = ${LYFLOW_PCL_LIBS}")

set(LYFLOW_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(LYFLOW_SRC     "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(LYFLOW_TP      "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
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
    # PCL/Eigen 在 /W4 下噪声极大，且不是我们能修的
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# 算子包（ADR-0013）。LYFLOW_OP_PACKS 是分号分隔的目录列表，每个目录下有一个
# lyflow_op_pack.cmake；不设就是普通构建。怎么写一个包见 docs/op-packs.md。
if(NOT DEFINED LYFLOW_OP_PACKS)
  set(LYFLOW_OP_PACKS "$ENV{LYFLOW_OP_PACKS}")
endif()

set(LYFLOW_PACK_NAMES "")
set(LYFLOW_PACK_SOURCES "")
set(LYFLOW_PACK_TEST_SOURCES "")
set(LYFLOW_PACK_INCLUDES "")
set(LYFLOW_PACK_LIBS "")
set(LYFLOW_PACK_OPTIONS "")
set(LYFLOW_PACK_DEFINES "")
set(LYFLOW_PACK_PCH "")

# 包在自己的 lyflow_op_pack.cmake 里调它。必须是 macro：函数里的 set 出不了作用域。
macro(lyflow_op_pack)
  set(_opts "")
  set(_one NAME PCH)
  set(_multi SOURCES TEST_SOURCES INCLUDES LINK OPTIONS DEFINES)
  cmake_parse_arguments(LYPACK "${_opts}" "${_one}" "${_multi}" ${ARGN})
  if(NOT LYPACK_NAME)
    message(FATAL_ERROR "lyflow_op_pack() 缺少 NAME")
  endif()
  # 所有包共用一个对象库，所以预编译头只能有一份。两个包都给就直接报错。
  if(LYPACK_PCH)
    if(LYFLOW_PACK_PCH AND NOT LYFLOW_PACK_PCH STREQUAL LYPACK_PCH)
      message(FATAL_ERROR "两个算子包给了不同的 PCH：${LYFLOW_PACK_PCH} 与 ${LYPACK_PCH}")
    endif()
    set(LYFLOW_PACK_PCH "${LYPACK_PCH}")
  endif()
  list(APPEND LYFLOW_PACK_NAMES ${LYPACK_NAME})
  list(APPEND LYFLOW_PACK_SOURCES ${LYPACK_SOURCES})
  list(APPEND LYFLOW_PACK_TEST_SOURCES ${LYPACK_TEST_SOURCES})
  list(APPEND LYFLOW_PACK_INCLUDES ${LYPACK_INCLUDES})
  list(APPEND LYFLOW_PACK_LIBS ${LYPACK_LINK})
  list(APPEND LYFLOW_PACK_OPTIONS ${LYPACK_OPTIONS})
  list(APPEND LYFLOW_PACK_DEFINES ${LYPACK_DEFINES})
endmacro()

foreach(_raw_pack IN LISTS LYFLOW_OP_PACKS)
  if(_raw_pack STREQUAL "")
    continue()
  endif()
  # 环境变量里是 Windows 的反斜杠路径，CMake 会把 \p 当成非法转义。
  file(TO_CMAKE_PATH "${_raw_pack}" pack)
  set(_pack_file "${pack}/lyflow_op_pack.cmake")
  if(NOT EXISTS "${_pack_file}")
    message(FATAL_ERROR "算子包目录 ${pack} 里没有 lyflow_op_pack.cmake")
  endif()
  set(LYFLOW_PACK_DIR "${pack}")
  include("${_pack_file}")
  # 包目录参与 configure 依赖：包里加了 .cpp 之后不用手动删缓存
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pack_file}")
endforeach()
message(STATUS "lyflow: op packs = ${LYFLOW_PACK_NAMES}")

# 注册入口是生成的：每个包出一个 lyflow::packs::<name>::registerPackOps。
# 手写一张表的话，加一个包要同时改 core 和包两边。
set(LYFLOW_PACK_DECLS "")
set(LYFLOW_PACK_CALLS "")
foreach(name IN LISTS LYFLOW_PACK_NAMES)
  string(APPEND LYFLOW_PACK_DECLS
         "namespace lyflow::packs::${name} { void registerPackOps(::lyflow::Registry&); }\n")
  string(APPEND LYFLOW_PACK_CALLS "  packs::${name}::registerPackOps(r);\n")
endforeach()
set(LYFLOW_PACK_REGISTRAR "${CMAKE_BINARY_DIR}/generated/op_packs.cpp")
# 先写 .in 再 COPYONLY：内容没变时 configure_file 不动时间戳，
# 否则每次 configure 都要重编这个 TU 并重链 DLL。
file(WRITE "${LYFLOW_PACK_REGISTRAR}.in"
"// 由 core/CMakeLists.txt 生成，勿手改。算子包机制见 docs/op-packs.md。
#include \"lyflow/registry.h\"

${LYFLOW_PACK_DECLS}
namespace lyflow {
void registerOpPacks(Registry& r) {
  (void)r;
${LYFLOW_PACK_CALLS}}
}  // namespace lyflow
")
configure_file("${LYFLOW_PACK_REGISTRAR}.in" "${LYFLOW_PACK_REGISTRAR}" COPYONLY)

# 两个对象库。分开的唯一理由是 PCH：只有 pcl/ 下的 TU 值得预编译那堆重头文件，
# 其余 TU 不该被拖慢（D2 的「加算子秒级反馈」）。
file(GLOB LYFLOW_CORE_SOURCES CONFIGURE_DEPENDS
  "${LYFLOW_SRC}/*.cpp" "${LYFLOW_SRC}/exec/*.cpp" "${LYFLOW_SRC}/ops/*.cpp")
file(GLOB LYFLOW_PCL_SOURCES CONFIGURE_DEPENDS
  "${LYFLOW_SRC}/ops/pcl/*.cpp")

add_library(lyflow_core_obj OBJECT ${LYFLOW_CORE_SOURCES} "${LYFLOW_PACK_REGISTRAR}")
lyflow_apply_common(lyflow_core_obj)

add_library(lyflow_core_pcl OBJECT ${LYFLOW_PCL_SOURCES})
lyflow_apply_common(lyflow_core_pcl)
target_link_libraries(lyflow_core_pcl PUBLIC
  ${LYFLOW_PCL_LIBS})
target_precompile_headers(lyflow_core_pcl PRIVATE "${LYFLOW_SRC}/ops/pcl/pcl_pch.h")
if(MSVC)
  # PCL/Eigen 自己的告警不是我们能修的，但我们自己的代码仍然 /W4
  target_compile_options(lyflow_core_pcl PRIVATE /wd4127 /wd4267 /wd4244 /wd4324)
endif()

# 算子包的对象库。没有包时是个空目标，链进去不产生任何符号。
add_library(lyflow_core_packs OBJECT
  "${LYFLOW_SRC}/packs/placeholder.cpp" ${LYFLOW_PACK_SOURCES})
lyflow_apply_common(lyflow_core_packs)
if(LYFLOW_PACK_SOURCES)
  target_include_directories(lyflow_core_packs PRIVATE ${LYFLOW_PACK_INCLUDES})
  target_link_libraries(lyflow_core_packs PUBLIC ${LYFLOW_PCL_LIBS} ${LYFLOW_PACK_LIBS})
  if(LYFLOW_PACK_DEFINES)
    target_compile_definitions(lyflow_core_packs PRIVATE ${LYFLOW_PACK_DEFINES})
  endif()
  if(MSVC)
    # 包里 include 的是别人家的头（PCL/Eigen/领域库），/W4 的噪声不是包作者能修的
    target_compile_options(lyflow_core_packs PRIVATE
      /wd4127 /wd4267 /wd4244 /wd4324 /wd4100 /wd4459)
  endif()
  if(LYFLOW_PACK_OPTIONS)
    target_compile_options(lyflow_core_packs PRIVATE ${LYFLOW_PACK_OPTIONS})
  endif()
  # 领域包的每个 TU 都要拖一遍 PCL + Eigen + 领域头，不预编译的话一次增量
  # 重建要十秒，热重载就跟不上手了。
  if(LYFLOW_PACK_PCH)
    target_precompile_headers(lyflow_core_packs PRIVATE "${LYFLOW_PACK_PCH}")
  endif()
endif()

# D1：core 是 DLL，只导出 C ABI。Rust 用 libloading 运行时加载。
add_library(lyflow_core SHARED
  $<TARGET_OBJECTS:lyflow_core_obj> $<TARGET_OBJECTS:lyflow_core_pcl>
  $<TARGET_OBJECTS:lyflow_core_packs>)
target_include_directories(lyflow_core PUBLIC "${LYFLOW_INCLUDE}")
target_link_libraries(lyflow_core PRIVATE
  ${LYFLOW_PCL_LIBS} ${LYFLOW_PACK_LIBS})
set_target_properties(lyflow_core PROPERTIES
  # Rust 侧按固定名字 LoadLibrary，不要带 lib 前缀或版本后缀
  OUTPUT_NAME "lyflow_core")

# 调试工具与测试。都直接吃对象库而不是链 DLL —— DLL 只导出 C ABI，
# 而这两个要用的是 Registry / Executor 这些 C++ 内部类。
add_executable(lyflow-dump-manifest
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/dump_manifest.cpp"
  "${LYFLOW_MANIFEST}"
  $<TARGET_OBJECTS:lyflow_core_obj> $<TARGET_OBJECTS:lyflow_core_pcl>
  $<TARGET_OBJECTS:lyflow_core_packs>)
lyflow_apply_common(lyflow-dump-manifest)
target_link_libraries(lyflow-dump-manifest PRIVATE
  ${LYFLOW_PCL_LIBS} ${LYFLOW_PACK_LIBS})

# 带包构建时把包自己的测试源也编进同一个 doctest 目标（不另起一个 exe）。
file(GLOB LYFLOW_TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
add_executable(lyflow-core-tests
  ${LYFLOW_TEST_SOURCES} ${LYFLOW_PACK_TEST_SOURCES}
  "${LYFLOW_MANIFEST}"
  $<TARGET_OBJECTS:lyflow_core_obj> $<TARGET_OBJECTS:lyflow_core_pcl>
  $<TARGET_OBJECTS:lyflow_core_packs>)
lyflow_apply_common(lyflow-core-tests)
if(LYFLOW_PACK_TEST_SOURCES)
  target_include_directories(lyflow-core-tests PRIVATE ${LYFLOW_PACK_INCLUDES})
endif()
if(MSVC)
  # doctest 为了 stringify 特化了 std::tuple，C++20 起标准禁止，MSVC 会警告。
  # 不是我们能修的第三方代码，单独关掉这一条，其余 /W4 照旧。
  target_compile_options(lyflow-core-tests PRIVATE /wd5285)
endif()
target_link_libraries(lyflow-core-tests PRIVATE
  ${LYFLOW_PCL_LIBS} ${LYFLOW_PACK_LIBS})

enable_testing()
add_test(NAME lyflow-core-tests COMMAND lyflow-core-tests)

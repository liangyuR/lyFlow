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

# 所有产物（DLL、exe、vcpkg applocal 拷来的依赖 DLL）都落在同一个 bin/。
# bridge/build.rs 整目录拷贝它，Rust 侧运行时就能 LoadLibrary 到 lyflow_core.dll
# 以及它依赖的那几十个 PCL/boost DLL。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set(LYFLOW_PCL_COMPONENTS common io filters kdtree search segmentation sample_consensus features)
find_package(PCL 1.12 REQUIRED COMPONENTS ${LYFLOW_PCL_COMPONENTS})

# PCL 的导出目标叫什么，取决于装的是哪一版：上游 PCLConfig.cmake 造的是
# 非命名空间的 pcl_common / pcl_io，而某些发行版（以及 PCL 自己的未来版本）
# 用 PCL::common。硬写一种就等着在别人机器上炸 —— 这里两种都认，
# 都没有时退回经典的 ${PCL_LIBRARIES} + ${PCL_INCLUDE_DIRS}。
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

# D9：exe 内嵌 activeCodePage=UTF-8。PCL 的文件 IO 走窄字符串，
# 没有这条的话中文路径在 ANSI 代码页下根本打不开。
# 只有 exe 的 manifest 才决定进程的 ACP —— DLL 上贴 manifest 无效。
function(lyflow_apply_common target)
  target_include_directories(${target} PRIVATE "${LYFLOW_INCLUDE}" "${LYFLOW_SRC}" "${LYFLOW_TP}")
  target_compile_definitions(${target} PRIVATE LYFLOW_BUILDING_DLL)
  if(MSVC)
    # /bigobj: exec/ 里模板实例化多；/utf-8: 源文件里有中文字面量，
    # 缺了它 MSVC 按系统代码页解释，manifest 里的中文到前端才暴露成乱码。
    # /EHsc 必须显式写：cmake crate 会整个替换 CMAKE_CXX_FLAGS，把 CMake 默认
    # 带的那个 /EHsc 顶掉。后果是 MSVC 静默进入「异常关闭」模式，而本项目
    # 到处依赖 try/catch —— c_api 的每个入口都要兜住异常，绝不让它跨 ABI。
    target_compile_options(${target} PRIVATE /EHsc /W4 /permissive- /utf-8 /bigobj)
    # PCL/Eigen 在 /W4 下噪声极大，且不是我们能修的
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# ---------------------------------------------------------------------------
# 两个对象库。分开的唯一理由是 PCH：只有 pcl/ 下的 TU 值得预编译那堆重头文件，
# 其余 TU 不该被拖慢（D2 的「加算子秒级反馈」）。
# ---------------------------------------------------------------------------
file(GLOB LYFLOW_CORE_SOURCES CONFIGURE_DEPENDS
  "${LYFLOW_SRC}/*.cpp" "${LYFLOW_SRC}/exec/*.cpp" "${LYFLOW_SRC}/ops/*.cpp")
file(GLOB LYFLOW_PCL_SOURCES CONFIGURE_DEPENDS
  "${LYFLOW_SRC}/ops/pcl/*.cpp")

add_library(lyflow_core_obj OBJECT ${LYFLOW_CORE_SOURCES})
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

# ---------------------------------------------------------------------------
# D1：core 是 DLL，只导出 C ABI。Rust 用 libloading 运行时加载。
# ---------------------------------------------------------------------------
add_library(lyflow_core SHARED
  $<TARGET_OBJECTS:lyflow_core_obj> $<TARGET_OBJECTS:lyflow_core_pcl>)
target_include_directories(lyflow_core PUBLIC "${LYFLOW_INCLUDE}")
target_link_libraries(lyflow_core PRIVATE
  ${LYFLOW_PCL_LIBS})
set_target_properties(lyflow_core PROPERTIES
  # Rust 侧按固定名字 LoadLibrary，不要带 lib 前缀或版本后缀
  OUTPUT_NAME "lyflow_core")

# ---------------------------------------------------------------------------
# 调试工具与测试。都直接吃对象库而不是链 DLL —— DLL 只导出 C ABI，
# 而这两个要用的是 Registry / Executor 这些 C++ 内部类。
# ---------------------------------------------------------------------------
add_executable(lyflow-dump-manifest
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/dump_manifest.cpp"
  "${LYFLOW_MANIFEST}"
  $<TARGET_OBJECTS:lyflow_core_obj> $<TARGET_OBJECTS:lyflow_core_pcl>)
lyflow_apply_common(lyflow-dump-manifest)
target_link_libraries(lyflow-dump-manifest PRIVATE
  ${LYFLOW_PCL_LIBS})

file(GLOB LYFLOW_TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
add_executable(lyflow-core-tests
  ${LYFLOW_TEST_SOURCES}
  "${LYFLOW_MANIFEST}"
  $<TARGET_OBJECTS:lyflow_core_obj> $<TARGET_OBJECTS:lyflow_core_pcl>)
lyflow_apply_common(lyflow-core-tests)
if(MSVC)
  # doctest 为了 stringify 特化了 std::tuple，C++20 起标准禁止，MSVC 会警告。
  # 不是我们能修的第三方代码，单独关掉这一条，其余 /W4 照旧。
  target_compile_options(lyflow-core-tests PRIVATE /wd5285)
endif()
target_link_libraries(lyflow-core-tests PRIVATE
  ${LYFLOW_PCL_LIBS})

enable_testing()
add_test(NAME lyflow-core-tests COMMAND lyflow-core-tests)

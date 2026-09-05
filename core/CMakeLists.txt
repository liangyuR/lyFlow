cmake_minimum_required(VERSION 3.20)
project(lyflow_core LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# M0 刻意零第三方依赖。这样「加算子 -> 前端节点面板出现」这条链路
# 的构建是秒级的，不会被 vcpkg 拉 boost/VTK 卡住。等 M2 真要跑点云
# 了再引 PCL —— 那时算子的 compute 才有实体，依赖才有意义。

file(GLOB LYFLOW_CORE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ops/*.cpp"
)

add_library(lyflow_core STATIC ${LYFLOW_CORE_SOURCES})

target_include_directories(lyflow_core
  PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

if(MSVC)
  target_compile_options(lyflow_core PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(lyflow_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# 调试工具：不启动 app 就能看见 C++ 导出了什么。
add_executable(lyflow-dump-manifest "${CMAKE_CURRENT_SOURCE_DIR}/tools/dump_manifest.cpp")
target_link_libraries(lyflow-dump-manifest PRIVATE lyflow_core)
if(MSVC)
  target_compile_options(lyflow-dump-manifest PRIVATE /W4 /permissive- /utf-8)
endif()

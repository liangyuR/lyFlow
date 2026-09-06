cmake_minimum_required(VERSION 3.20)

# 独立的消费方工程：只 find_package(lyflow)，不看 LyFlow 的源码树。
# pnpm check 用它验证 cmake --install 的产物真的能被外部程序用起来（A1-11）。
project(lyflow_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(lyflow CONFIG REQUIRED)

foreach(sample embed_minimal embed_concurrent embed_inject)
  add_executable(${sample} "${LYFLOW_EXAMPLES_DIR}/${sample}.cpp")
  target_link_libraries(${sample} PRIVATE lyflow::client)
  if(MSVC)
    target_compile_options(${sample} PRIVATE /EHsc /W4 /utf-8 /permissive-)
  endif()
endforeach()

# core DLL 与它的依赖都在 <prefix>/bin。拷到 exe 边上，运行时不必设 PATH。
add_custom_command(TARGET embed_minimal POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${LYFLOW_BIN_DIR}" "$<TARGET_FILE_DIR:embed_minimal>")

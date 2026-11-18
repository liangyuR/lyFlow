cmake_minimum_required(VERSION 3.20)
project(lyflow_public_kernels LANGUAGES CXX)

find_package(lyflow 2.0.2 EXACT REQUIRED COMPONENTS geometry_kernel metrology_kernel profile_kernel profile_analysis_kernel)

add_executable(lyflow-public-kernels ../public_kernels.cpp)
target_compile_features(lyflow-public-kernels PRIVATE cxx_std_17)
target_link_libraries(lyflow-public-kernels PRIVATE lyflow::geometry_kernel lyflow::metrology_kernel lyflow::profile_kernel lyflow::profile_analysis_kernel)

if(MSVC)
  target_compile_options(lyflow-public-kernels PRIVATE /utf-8 /EHsc)
endif()

enable_testing()
add_test(NAME public-kernels COMMAND lyflow-public-kernels)

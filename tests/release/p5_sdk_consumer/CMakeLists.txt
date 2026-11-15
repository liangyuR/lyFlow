cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED LYFLOW_CONSUMER_VERSION OR
   NOT LYFLOW_CONSUMER_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "LYFLOW_CONSUMER_VERSION must be an explicit X.Y.Z version")
endif()

project(lyflow_p5_sdk_consumer LANGUAGES CXX)
find_package(lyflow ${LYFLOW_CONSUMER_VERSION} EXACT CONFIG REQUIRED)
add_executable(p5_sdk_consumer main.cpp)
target_link_libraries(p5_sdk_consumer PRIVATE lyflow::client)
target_compile_features(p5_sdk_consumer PRIVATE cxx_std_17)
set_target_properties(p5_sdk_consumer PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS OFF)
target_compile_definitions(p5_sdk_consumer PRIVATE
  LYFLOW_EXPECTED_VERSION="${LYFLOW_CONSUMER_VERSION}")
if(MSVC)
  target_compile_options(p5_sdk_consumer PRIVATE /EHsc /utf-8)
endif()

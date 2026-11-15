cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED LYFLOW_CONSUMER_VERSION OR
   NOT LYFLOW_CONSUMER_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "LYFLOW_CONSUMER_VERSION must be an explicit X.Y.Z version")
endif()

project(lyflow_p5_sdk_consumer LANGUAGES CXX)
find_package(lyflow ${LYFLOW_CONSUMER_VERSION} EXACT CONFIG REQUIRED)
add_library(profile_sdk_headers OBJECT profile_headers.cpp)
target_link_libraries(profile_sdk_headers PRIVATE lyflow::client)
add_executable(profile_sdk_consumer "${LYFLOW_EXAMPLES_DIR}/embed_profile.cpp")
target_link_libraries(profile_sdk_consumer PRIVATE lyflow::client)
set_target_properties(profile_sdk_consumer PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
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
  target_compile_options(profile_sdk_consumer PRIVATE /EHsc /utf-8)
  target_compile_options(profile_sdk_headers PRIVATE /EHsc /utf-8)
endif()

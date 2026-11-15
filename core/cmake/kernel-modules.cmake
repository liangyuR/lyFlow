include_guard(GLOBAL)
get_filename_component(LYFLOW_KERNEL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../kernels" ABSOLUTE)
set(LYFLOW_KERNEL_ROOT "${LYFLOW_KERNEL_ROOT}" CACHE INTERNAL "Public portable kernel sources")

function(lyflow_setup_data_contract)
  add_library(lyflow_data_contract INTERFACE)
  add_library(lyflow::data_contract ALIAS lyflow_data_contract)
  set_target_properties(lyflow_data_contract PROPERTIES EXPORT_NAME data_contract)
  target_include_directories(lyflow_data_contract INTERFACE
    "$<BUILD_INTERFACE:${LYFLOW_INCLUDE}>" "$<BUILD_INTERFACE:${LYFLOW_TP}>"
    "$<INSTALL_INTERFACE:include>")
  target_compile_features(lyflow_data_contract INTERFACE cxx_std_17)
  install(TARGETS lyflow_data_contract EXPORT lyflow-kernel-targets)
endfunction()

function(lyflow_kernel_target target public_name)
  add_library(${target} STATIC ${ARGN})
  add_library(lyflow::${public_name} ALIAS ${target})
  set_target_properties(${target} PROPERTIES EXPORT_NAME ${public_name} POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(${target} PUBLIC lyflow_data_contract)
  target_include_directories(${target} PUBLIC
    "$<BUILD_INTERFACE:${LYFLOW_KERNEL_ROOT}/include>" "$<INSTALL_INTERFACE:include>")
  if(MSVC)
    target_compile_options(${target} PRIVATE /EHsc /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(${target} PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  install(TARGETS ${target} EXPORT lyflow-kernel-targets ARCHIVE DESTINATION lib)
endfunction()

function(lyflow_setup_geometry_kernel)
  lyflow_kernel_target(lyflow_geometry_kernel geometry_kernel
    "${LYFLOW_KERNEL_ROOT}/src/geometry.cpp" "${LYFLOW_KERNEL_ROOT}/src/rigid.cpp"
    "${LYFLOW_KERNEL_ROOT}/src/constraints.cpp")
endfunction()

function(lyflow_setup_metrology_kernel)
  lyflow_kernel_target(lyflow_metrology_kernel metrology_kernel "${LYFLOW_KERNEL_ROOT}/src/metrology.cpp")
endfunction()

lyflow_declare_module(NAME data-contract TARGET lyflow_data_contract SETUP lyflow_setup_data_contract)
lyflow_declare_module(NAME geometry-kernel TARGET lyflow_geometry_kernel SETUP lyflow_setup_geometry_kernel
  REQUIRES data-contract)
lyflow_declare_module(NAME metrology-kernel TARGET lyflow_metrology_kernel SETUP lyflow_setup_metrology_kernel
  REQUIRES data-contract)

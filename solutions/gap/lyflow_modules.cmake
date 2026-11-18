function(_lyflow_gap_static target)
  add_library(${target} STATIC ${ARGN})
  lyflow_apply_common(${target})
  set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/algo")
  if(MSVC)
    target_compile_options(${target} PRIVATE /wd4996 /wd4244 /wd4267)
    target_compile_definitions(${target} PRIVATE _USE_MATH_DEFINES)
  endif()
endfunction()

function(lyflow_setup_gap_geometry)
  _lyflow_gap_static(lyflow_gap_geometry
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/algo/gap_detection/GapUtils.cpp")
  target_link_libraries(lyflow_gap_geometry PUBLIC lyflow_std_algo lyflow_data_contract)
endfunction()

function(lyflow_setup_gap_template)
  _lyflow_gap_static(lyflow_gap_template
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/algo/gap_detection/Alignment.cpp")
  target_link_libraries(lyflow_gap_template PUBLIC lyflow_gap_geometry)
endfunction()

function(lyflow_setup_gap_model_adapters)
  add_library(lyflow_gap_model_adapters STATIC
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/algo/gap_ml/RoiFeatures.cpp"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/algo/gap_ml/RoiBoxes.cpp")
  lyflow_apply_common(lyflow_gap_model_adapters)
  set_target_properties(lyflow_gap_model_adapters PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(lyflow_gap_model_adapters PUBLIC "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/algo")
  target_link_libraries(lyflow_gap_model_adapters PUBLIC lyflow_data_contract)
endfunction()

function(lyflow_setup_gap_model)
  add_library(lyflow_gap_model INTERFACE)
  target_link_libraries(lyflow_gap_model INTERFACE lyflow_gap_model_adapters lyflow_ml_runtime)
endfunction()

function(lyflow_setup_gap_import)
  add_library(lyflow_gap_import INTERFACE)
  target_link_libraries(lyflow_gap_import INTERFACE lyflow_yaml_support)
endfunction()

lyflow_declare_module(NAME gap-geometry TARGET lyflow_gap_geometry SETUP lyflow_setup_gap_geometry
  REQUIRES pointcloud-algorithms data-contract)
lyflow_declare_module(NAME gap-template TARGET lyflow_gap_template SETUP lyflow_setup_gap_template
  REQUIRES gap-geometry)
lyflow_declare_module(NAME gap-model-adapters TARGET lyflow_gap_model_adapters SETUP lyflow_setup_gap_model_adapters
  REQUIRES data-contract)
lyflow_declare_module(NAME gap-model TARGET lyflow_gap_model SETUP lyflow_setup_gap_model
  REQUIRES gap-model-adapters ml-runtime)
lyflow_declare_module(NAME gap-import TARGET lyflow_gap_import SETUP lyflow_setup_gap_import
  REQUIRES yaml-support)

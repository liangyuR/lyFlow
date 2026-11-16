include_guard(GLOBAL)
get_filename_component(LYFLOW_SOLUTION_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../solutions" ABSOLUTE)

function(lyflow_setup_gap_geometry)
  add_library(lyflow_gap_geometry STATIC
    "${LYFLOW_SOLUTION_ROOT}/gap/algo/gap_detection/GapUtils.cpp"
    "${LYFLOW_SOLUTION_ROOT}/gap/algo/gap_detection/Converter.cpp")
  lyflow_apply_common(lyflow_gap_geometry)
  set_target_properties(lyflow_gap_geometry PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(lyflow_gap_geometry PUBLIC "${LYFLOW_SOLUTION_ROOT}/gap/algo")
  target_link_libraries(lyflow_gap_geometry PUBLIC lyflow_std_algo lyflow_data_contract)
  if(MSVC)
    target_compile_options(lyflow_gap_geometry PRIVATE /wd4996 /wd4244 /wd4267)
    target_compile_definitions(lyflow_gap_geometry PRIVATE _USE_MATH_DEFINES)
  endif()
endfunction()

function(lyflow_setup_gap_template)
  add_library(lyflow_gap_template STATIC
    "${LYFLOW_SOLUTION_ROOT}/gap/algo/gap_detection/Alignment.cpp")
  lyflow_apply_common(lyflow_gap_template)
  set_target_properties(lyflow_gap_template PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(lyflow_gap_template PUBLIC "${LYFLOW_SOLUTION_ROOT}/gap/algo")
  target_link_libraries(lyflow_gap_template PUBLIC lyflow_gap_geometry)
  if(MSVC)
    target_compile_options(lyflow_gap_template PRIVATE /wd4996 /wd4244 /wd4267)
    target_compile_definitions(lyflow_gap_template PRIVATE _USE_MATH_DEFINES)
  endif()
endfunction()

function(lyflow_setup_gap_model_adapters)
  add_library(lyflow_gap_model_adapters STATIC
    "${LYFLOW_SOLUTION_ROOT}/gap/algo/gap_ml/RoiFeatures.cpp"
    "${LYFLOW_SOLUTION_ROOT}/gap/algo/gap_ml/RoiBoxes.cpp")
  lyflow_apply_common(lyflow_gap_model_adapters)
  set_target_properties(lyflow_gap_model_adapters PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(lyflow_gap_model_adapters PUBLIC "${LYFLOW_SOLUTION_ROOT}/gap/algo")
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

function(lyflow_setup_gap_compat)
  add_library(lyflow_gap_compat STATIC
    "${LYFLOW_SOLUTION_ROOT}/gap/algo/gap_core/MeasurementEngine.cpp"
    "${LYFLOW_SOLUTION_ROOT}/gap/algo/gap_core/MeasurementTypes.cpp"
    "${LYFLOW_SOLUTION_ROOT}/gap/algo/gap_detection/GapDetection.cpp"
    "${LYFLOW_SOLUTION_ROOT}/gap/algo/gap_detection/DetectionConfigurationAdapter.cpp"
    "${LYFLOW_SOLUTION_ROOT}/gap/algo/gap_ml/OnnxRoiPredictor.cpp"
    "${LYFLOW_SOLUTION_ROOT}/gap/algo/gap_io/Sha256.cpp")
  lyflow_apply_common(lyflow_gap_compat)
  set_target_properties(lyflow_gap_compat PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(lyflow_gap_compat PUBLIC "${LYFLOW_SOLUTION_ROOT}/gap/algo")
  target_link_libraries(lyflow_gap_compat PUBLIC lyflow_gap_template lyflow_gap_model lyflow_gap_import
    PRIVATE lyflow_onnx_support)
  if(MSVC)
    target_compile_options(lyflow_gap_compat PRIVATE /wd4996 /wd4244 /wd4267 /wd4275)
    target_compile_definitions(lyflow_gap_compat PRIVATE _USE_MATH_DEFINES)
  endif()
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
lyflow_declare_module(NAME gap-compat TARGET lyflow_gap_compat SETUP lyflow_setup_gap_compat
  REQUIRES gap-template gap-model gap-import onnx-support)

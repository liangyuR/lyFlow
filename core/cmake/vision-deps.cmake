include_guard(GLOBAL)

function(lyflow_configure_marker_dictionary)
  if(TARGET lyflow_marker_dictionary)
    return()
  endif()
  get_filename_component(_repository "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.." ABSOLUTE)
  set(LYFLOW_VISION_DEPS_DIR "${_repository}/build/vision-deps" CACHE PATH
      "Prepared pinned OpenCV and STag dictionary sources")
  if(NOT EXISTS "${LYFLOW_VISION_DEPS_DIR}/prepared.json")
    message(FATAL_ERROR "Run python scripts/fetch-vision-deps.py before building std-image.")
  endif()
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_repository}/scripts/fetch-vision-deps.py"
            --output-dir "${LYFLOW_VISION_DEPS_DIR}" --verify-only --offline
    RESULT_VARIABLE _verified
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _error
  )
  if(NOT _verified EQUAL 0)
    message(FATAL_ERROR "Pinned vision source verification failed: ${_output}${_error}")
  endif()
  add_library(lyflow_marker_dictionary INTERFACE)
  target_include_directories(lyflow_marker_dictionary SYSTEM INTERFACE "${LYFLOW_VISION_DEPS_DIR}/stag/src")
endfunction()

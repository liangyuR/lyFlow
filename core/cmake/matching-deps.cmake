include_guard(GLOBAL)

function(lyflow_configure_matching_cpu)
  if(TARGET lyflow_matching_cpu)
    return()
  endif()
  get_filename_component(_repository "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.." ABSOLUTE)
  set(LYFLOW_MATCHING_DEPS_DIR "${_repository}/build/matching-deps" CACHE PATH
      "Prepared, pinned KISS-Matcher/small_gicp CPU dependencies")
  if(NOT EXISTS "${LYFLOW_MATCHING_DEPS_DIR}/prepared.json")
    message(FATAL_ERROR
      "CPU matching sources are required by std-pointcloud. Run python scripts/fetch-matching-deps.py --output-dir \"${LYFLOW_MATCHING_DEPS_DIR}\" first.")
  endif()
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_repository}/scripts/fetch-matching-deps.py"
            --output-dir "${LYFLOW_MATCHING_DEPS_DIR}" --verify-only --offline
    RESULT_VARIABLE _verified
    OUTPUT_VARIABLE _verify_output
    ERROR_VARIABLE _verify_error
  )
  if(NOT _verified EQUAL 0)
    message(FATAL_ERROR "Pinned CPU matching source verification failed: ${_verify_output}${_verify_error}")
  endif()

  find_package(Eigen3 CONFIG REQUIRED)
  find_package(flann CONFIG REQUIRED)
  find_package(lz4 CONFIG REQUIRED)
  if(MSVC)
    set(OpenMP_RUNTIME_MSVC "")
  endif()
  find_package(OpenMP REQUIRED COMPONENTS CXX)
  get_target_property(_openmp_options OpenMP::OpenMP_CXX INTERFACE_COMPILE_OPTIONS)
  if(MSVC AND (OpenMP_CXX_FLAGS MATCHES "llvm|experimental" OR _openmp_options MATCHES "llvm|experimental"))
    message(FATAL_ERROR "CPU matching requires classic MSVC /openmp and the redistributable vcomp runtime; clear cached LLVM OpenMP flags.")
  endif()

  set(_kiss "${LYFLOW_MATCHING_DEPS_DIR}/kiss-matcher/cpp/kiss_matcher/core")
  set(_robin "${LYFLOW_MATCHING_DEPS_DIR}/robin")
  set(_gicp "${LYFLOW_MATCHING_DEPS_DIR}/small-gicp")
  set(_shim "${_repository}/packs/std-pointcloud/algo")

  if(TARGET tbb OR TARGET TBB::tbb)
    message(FATAL_ERROR "CPU matching must build the pinned oneTBB source, but a conflicting TBB target already exists.")
  endif()
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  set(BUILD_SHARED_LIBS ON)
  set(TBB_TEST OFF)
  set(TBB_EXAMPLES OFF)
  set(TBB_STRICT OFF)
  set(TBBMALLOC_BUILD OFF)
  set(TBBMALLOC_PROXY_BUILD OFF)
  set(TBB4PY_BUILD OFF)
  set(TCM_BUILD OFF)
  set(TCM_TEST OFF)
  set(TBB_INSTALL OFF)
  set(TBB_FIND_PACKAGE OFF)
  set(TBB_ENABLE_IPO OFF)
  set(TBB_DISABLE_HWLOC_AUTOMATIC_SEARCH ON)
  add_subdirectory("${LYFLOW_MATCHING_DEPS_DIR}/tbb" "${CMAKE_BINARY_DIR}/matching/tbb" EXCLUDE_FROM_ALL)
  set_target_properties(tbb PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  )
  foreach(_configuration Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${_configuration}" _upper)
    set_target_properties(tbb PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_${_upper} "${CMAKE_BINARY_DIR}/bin"
      LIBRARY_OUTPUT_DIRECTORY_${_upper} "${CMAKE_BINARY_DIR}/bin"
      ARCHIVE_OUTPUT_DIRECTORY_${_upper} "${CMAKE_BINARY_DIR}/lib"
      PDB_OUTPUT_DIRECTORY_${_upper} "${CMAKE_BINARY_DIR}/bin"
    )
  endforeach()

  add_library(lyflow_matching_robin STATIC
    "${_robin}/src/core.cpp"
    "${_robin}/src/graph.cpp"
    "${_robin}/src/graph_core.cpp"
    "${_robin}/src/graph_io.cpp"
    "${_robin}/src/graph_solvers.cpp"
    "${_robin}/src/pkc.cpp"
    "${_robin}/src/problems.cpp"
    "${_robin}/src/robin.cpp"
    "${_robin}/src/utils.cpp"
  )
  target_include_directories(lyflow_matching_robin SYSTEM PUBLIC
    "${_robin}/include" "${LYFLOW_MATCHING_DEPS_DIR}/xenium" "${_shim}")
  target_link_libraries(lyflow_matching_robin PUBLIC Eigen3::Eigen OpenMP::OpenMP_CXX)

  add_library(lyflow_matching_kiss STATIC
    "${_kiss}/kiss_matcher/ROBINMatching.cpp"
    "${_kiss}/kiss_matcher/FasterPFH.cpp"
    "${_kiss}/kiss_matcher/KISSMatcher.cpp"
    "${_kiss}/kiss_matcher/GncSolver.cpp"
  )
  target_include_directories(lyflow_matching_kiss SYSTEM PUBLIC "${_kiss}")
  target_link_libraries(lyflow_matching_kiss PUBLIC
    lyflow_matching_robin flann::flann_cpp lz4::lz4 TBB::tbb)

  add_library(lyflow_matching_small_gicp STATIC
    "${_gicp}/src/small_gicp/registration/registration.cpp"
    "${_gicp}/src/small_gicp/registration/registration_helper.cpp"
  )
  target_include_directories(lyflow_matching_small_gicp SYSTEM PUBLIC "${_gicp}/include")
  target_link_libraries(lyflow_matching_small_gicp PUBLIC Eigen3::Eigen OpenMP::OpenMP_CXX)
  foreach(_target lyflow_matching_robin lyflow_matching_kiss lyflow_matching_small_gicp)
    set_target_properties(${_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(${_target} PUBLIC NOMINMAX _USE_MATH_DEFINES)
    if(MSVC)
      target_compile_options(${_target} PRIVATE /EHsc /bigobj /utf-8)
    endif()
  endforeach()

  add_library(lyflow_matching_cpu INTERFACE)
  target_link_libraries(lyflow_matching_cpu INTERFACE lyflow_matching_kiss lyflow_matching_small_gicp)
  target_compile_definitions(lyflow_matching_cpu INTERFACE LYFLOW_WITH_KISS_MATCHER=1)
  install(TARGETS tbb RUNTIME DESTINATION bin LIBRARY DESTINATION bin)
endfunction()

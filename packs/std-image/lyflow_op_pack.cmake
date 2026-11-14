# LyFlow 标准算子包：图像域的通用算子（docs/image-plan.md 阶段 2，ADR-0026）。
# 全仓库唯一一处 find_package(OpenCV)。边界照 ADR-0005：opencv2/ 的头只在这个包里出现，
# 进出 OpenCV 一律经 include/lyflow_cv/adapter.h。
#
# 默认开（image-plan Q3）：找不到 OpenCV 就 FATAL，与 std-pointcloud 对 PCL 的态度一致 ——
# 缺依赖的标准包静默跳过，只会让「图里的算子怎么没了」在很远的地方才暴露（ADR-0015 T9）。

find_package(OpenCV CONFIG QUIET COMPONENTS core imgproc imgcodecs objdetect)
if(NOT OpenCV_FOUND OR OpenCV_VERSION VERSION_LESS "4.6.0")
  message(FATAL_ERROR
    "packs/std-image requires OpenCV core / imgproc / imgcodecs / geometry / objdetect.\n"
    "Run scripts/build-vision-deps.ps1 and pass its OpenCV_DIR to CMake.\n"
    "或者纯平台构建时不编标准包（LYFLOW_STD_PACKS=0）。")
endif()
message(STATUS "lyflow: OpenCV ${OpenCV_VERSION}")
if(OpenCV_VERSION VERSION_LESS "5.0.0")
  find_package(OpenCV 4.6 CONFIG REQUIRED COMPONENTS core imgproc imgcodecs calib3d objdetect)
  set(LYFLOW_OPENCV_GEOMETRY_TARGET opencv_calib3d)
else()
  find_package(OpenCV 5.1 CONFIG REQUIRED COMPONENTS core imgproc imgcodecs geometry objdetect)
  set(LYFLOW_OPENCV_GEOMETRY_TARGET opencv_geometry)
endif()
set(LYFLOW_MARKER_OPENCV_TARGETS opencv_objdetect)
find_package(yaml-cpp CONFIG REQUIRED)
if(TARGET yaml-cpp::yaml-cpp)
  set(LYFLOW_WORKSPACE_YAML_TARGET yaml-cpp::yaml-cpp)
else()
  set(LYFLOW_WORKSPACE_YAML_TARGET yaml-cpp)
endif()

set(_lyflow_opencv_targets ${OpenCV_LIBS} opencv_flann opencv_features opencv_features2d)
foreach(_target IN LISTS _lyflow_opencv_targets)
  if(TARGET ${_target})
    get_target_property(_configurations ${_target} IMPORTED_CONFIGURATIONS)
    if("RELWITHDEBINFO" IN_LIST _configurations AND NOT "RELEASE" IN_LIST _configurations)
      set_target_properties(${_target} PROPERTIES MAP_IMPORTED_CONFIG_RELWITHDEBINFO RelWithDebInfo
                            MAP_IMPORTED_CONFIG_RELEASE RelWithDebInfo MAP_IMPORTED_CONFIG_MINSIZEREL RelWithDebInfo)
    endif()
    if(WIN32)
      get_target_property(_kind ${_target} TYPE)
      if(_kind STREQUAL "SHARED_LIBRARY")
        unset(_opencv_dll)
        foreach(_configuration RELWITHDEBINFO RELEASE)
          if(_configuration IN_LIST _configurations)
            get_target_property(_candidate ${_target} IMPORTED_LOCATION_${_configuration})
            if(EXISTS "${_candidate}")
              set(_opencv_dll "${_candidate}")
              break()
            endif()
          endif()
        endforeach()
        if(NOT _opencv_dll)
          message(FATAL_ERROR "${_target} requires a Release or RelWithDebInfo DLL for the dynamic CRT build.")
        endif()
        file(COPY "${_opencv_dll}" DESTINATION "${CMAKE_BINARY_DIR}/bin")
      endif()
    endif()
  endif()
endforeach()
include("${LYFLOW_PACK_DIR}/../../core/cmake/vision-deps.cmake")
lyflow_configure_marker_dictionary()
if(OpenCV_VERSION VERSION_LESS "4.7.0")
  find_package(OpenCV 4.6 CONFIG REQUIRED COMPONENTS core imgproc imgcodecs calib3d objdetect aruco)
  list(APPEND LYFLOW_MARKER_OPENCV_TARGETS opencv_aruco)
endif()

# 领域包共用图像适配层，不重复查找 OpenCV 或引入第二套 Image 模型。
add_library(lyflow_opencv_support INTERFACE)
target_include_directories(lyflow_opencv_support INTERFACE "${LYFLOW_PACK_DIR}/include")
target_link_libraries(lyflow_opencv_support INTERFACE opencv_core opencv_imgproc opencv_imgcodecs ${LYFLOW_OPENCV_GEOMETRY_TARGET} ${LYFLOW_MARKER_OPENCV_TARGETS} ${LYFLOW_WORKSPACE_YAML_TARGET} lyflow_marker_dictionary)

file(GLOB STD_IMAGE_SOURCES CONFIGURE_DEPENDS
  "${LYFLOW_PACK_DIR}/ops/*.cpp" "${LYFLOW_PACK_DIR}/src/*.cpp")
file(GLOB STD_IMAGE_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")

lyflow_op_pack(
  NAME         std-image
  VERSION      0.6.0
  DEFAULT      ON
  SOURCES      ${STD_IMAGE_SOURCES}
  TEST_SOURCES ${STD_IMAGE_TESTS}
  PCH          "${LYFLOW_PACK_DIR}/include/lyflow_cv/cv_pch.h"
  INCLUDES     "${LYFLOW_PACK_DIR}/include" "${LYFLOW_PACK_DIR}"
  LINK         lyflow_opencv_support
)

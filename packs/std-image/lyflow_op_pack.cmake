# LyFlow 标准算子包：图像域的通用算子（docs/image-plan.md 阶段 2，ADR-0026）。
# 全仓库唯一一处 find_package(OpenCV)。边界照 ADR-0005：opencv2/ 的头只在这个包里出现，
# 进出 OpenCV 一律经 include/lyflow_cv/adapter.h。
#
# 默认开（image-plan Q3）：找不到 OpenCV 就 FATAL，与 std-pointcloud 对 PCL 的态度一致 ——
# 缺依赖的标准包静默跳过，只会让「图里的算子怎么没了」在很远的地方才暴露（ADR-0015 T9）。

find_package(OpenCV 4 CONFIG QUIET COMPONENTS core imgproc imgcodecs)
if(NOT OpenCV_FOUND)
  message(FATAL_ERROR
    "packs/std-image 要 OpenCV 4（core / imgproc / imgcodecs），vcpkg 里没找到。\n"
    "先跑：vcpkg install opencv4:x64-windows\n"
    "或者纯平台构建时不编标准包（LYFLOW_STD_PACKS=0）。")
endif()
message(STATUS "lyflow: OpenCV ${OpenCV_VERSION}")

file(GLOB STD_IMAGE_SOURCES CONFIGURE_DEPENDS
  "${LYFLOW_PACK_DIR}/ops/*.cpp" "${LYFLOW_PACK_DIR}/src/*.cpp")
file(GLOB STD_IMAGE_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")

lyflow_op_pack(
  NAME         std-image
  VERSION      0.1.0
  DEFAULT      ON
  SOURCES      ${STD_IMAGE_SOURCES}
  TEST_SOURCES ${STD_IMAGE_TESTS}
  PCH          "${LYFLOW_PACK_DIR}/include/lyflow_cv/cv_pch.h"
  INCLUDES     "${LYFLOW_PACK_DIR}/include" "${LYFLOW_PACK_DIR}"
  LINK         opencv_core opencv_imgproc opencv_imgcodecs
)

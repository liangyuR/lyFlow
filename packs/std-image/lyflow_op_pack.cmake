file(GLOB STD_IMAGE_SOURCES CONFIGURE_DEPENDS
  "${LYFLOW_PACK_DIR}/ops/*.cpp")
file(GLOB STD_IMAGE_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")

lyflow_op_pack(
  NAME         std-image
  VERSION      0.12.0
  DEFAULT      ON
  SOURCES      ${STD_IMAGE_SOURCES}
  TEST_SOURCES ${STD_IMAGE_TESTS}
  PCH          "${LYFLOW_PACK_DIR}/include/lyflow_cv/cv_pch.h"
  INCLUDES     "${LYFLOW_PACK_DIR}/include" "${LYFLOW_PACK_DIR}"
  LINK         lyflow_opencv_support lyflow_yaml_support lyflow_marker_dictionary lyflow_barcode_backend lyflow_geometry_kernel
)

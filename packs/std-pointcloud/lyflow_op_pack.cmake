file(GLOB STD_PC_SOURCES CONFIGURE_DEPENDS
  "${LYFLOW_PACK_DIR}/ops/*.cpp")
file(GLOB STD_PC_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")

lyflow_op_pack(
  NAME         std-pointcloud
  VERSION      0.6.0
  DEFAULT      ON
  SOURCES      ${STD_PC_SOURCES}
  TEST_SOURCES ${STD_PC_TESTS}
  PCH          "${LYFLOW_PACK_DIR}/include/lyflow_pcl/pcl_pch.h"
  LINK         lyflow_pcl_support lyflow_std_algo lyflow_matching_algorithms lyflow_geometry_kernel lyflow_metrology_kernel
  # Eigen 5 把 jacobiSvd 的旧签名标了 deprecated，而迁来的 ICP 用的就是它
  OPTIONS      /wd4996
)

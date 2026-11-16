file(GLOB STD_METROLOGY_SOURCES CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/ops/*.cpp")
file(GLOB STD_METROLOGY_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")
lyflow_op_pack(
  NAME std-metrology
  VERSION 0.3.0
  DEFAULT ON
  SOURCES ${STD_METROLOGY_SOURCES}
  TEST_SOURCES ${STD_METROLOGY_TESTS}
  LINK lyflow_profile_contract lyflow_metrology_kernel lyflow_profile_analysis_kernel
)

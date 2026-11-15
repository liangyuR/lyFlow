file(GLOB STD_PROFILE_SOURCES CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/ops/*.cpp")
file(GLOB STD_PROFILE_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")

lyflow_op_pack(
  NAME std-profile
  VERSION 0.2.0
  DEFAULT ON
  SOURCES ${STD_PROFILE_SOURCES}
  TEST_SOURCES ${STD_PROFILE_TESTS}
  LINK lyflow_profile_contract lyflow_profile_kernel lyflow_profile_analysis_kernel
)

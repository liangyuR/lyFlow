file(GLOB STD_PROFILE_SOURCES CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/ops/*.cpp")
file(GLOB STD_PROFILE_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")
set(_profile_links lyflow_profile_contract lyflow_profile_kernel lyflow_profile_analysis_kernel)
if(TARGET lyflow_profile_robust_kernel)
  list(APPEND _profile_links lyflow_profile_robust_kernel)
else()
  list(REMOVE_ITEM STD_PROFILE_SOURCES "${LYFLOW_PACK_DIR}/ops/robust_features.cpp")
  list(REMOVE_ITEM STD_PROFILE_TESTS "${LYFLOW_PACK_DIR}/tests/test_robust_features.cpp")
endif()
if(TARGET lyflow_pcl_support)
  list(APPEND _profile_links lyflow_pcl_support)
else()
  list(REMOVE_ITEM STD_PROFILE_SOURCES "${LYFLOW_PACK_DIR}/ops/radius_outlier.cpp")
  list(REMOVE_ITEM STD_PROFILE_TESTS "${LYFLOW_PACK_DIR}/tests/test_radius_outlier.cpp")
endif()

lyflow_op_pack(
  NAME std-profile
  VERSION 0.4.0
  DEFAULT ON
  SOURCES ${STD_PROFILE_SOURCES}
  TEST_SOURCES ${STD_PROFILE_TESTS}
  LINK ${_profile_links}
)
if(TARGET lyflow_profile_robust_kernel)
  target_compile_definitions(lyflow_pack_std_profile PRIVATE LYFLOW_PROFILE_ROBUST=1)
endif()
if(TARGET lyflow_pcl_support)
  target_compile_definitions(lyflow_pack_std_profile PRIVATE LYFLOW_PROFILE_PCL_FILTER=1)
endif()

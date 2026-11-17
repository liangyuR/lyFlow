file(GLOB DTS_SOURCES CONFIGURE_DEPENDS
  "${LYFLOW_PACK_DIR}/ops/*.cpp" "${LYFLOW_PACK_DIR}/algo/*.cpp")
file(GLOB DTS_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")

lyflow_op_pack(
  NAME         solution-dts
  VERSION      0.1.0
  DEFAULT      OFF
  SOURCES      ${DTS_SOURCES}
  TEST_SOURCES ${DTS_TESTS}
  INCLUDES     "${LYFLOW_PACK_DIR}"
  LINK lyflow_profile_kernel lyflow_profile_contract lyflow_metrology_kernel
)

install(DIRECTORY "${LYFLOW_PACK_DIR}/graphs/" DESTINATION examples/dts)
install(FILES "${LYFLOW_PACK_DIR}/README.md" DESTINATION examples/dts)
install(FILES "${LYFLOW_PACK_DIR}/../../docs/dts-profile-migration.md" DESTINATION docs)

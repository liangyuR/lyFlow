file(GLOB STD_GEOMETRY_SOURCES CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/ops/*.cpp")
file(GLOB STD_GEOMETRY_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")

set(STD_GEOMETRY_LIBS lyflow_geometry_kernel lyflow_profile_contract)
set(STD_GEOMETRY_DEFINES)
if(TARGET lyflow_workspace_io)
  list(APPEND STD_GEOMETRY_LIBS lyflow_workspace_io)
  list(APPEND STD_GEOMETRY_DEFINES LYFLOW_WORKSPACE_IO=1)
else()
  list(REMOVE_ITEM STD_GEOMETRY_SOURCES "${LYFLOW_PACK_DIR}/ops/p3_workspace.cpp")
  list(REMOVE_ITEM STD_GEOMETRY_TESTS "${LYFLOW_PACK_DIR}/tests/test_p3_workspace.cpp")
endif()

lyflow_op_pack(
  NAME         std-geometry
  VERSION      0.9.0
  DEFAULT      ON
  SOURCES      ${STD_GEOMETRY_SOURCES}
  TEST_SOURCES ${STD_GEOMETRY_TESTS}
  LINK         ${STD_GEOMETRY_LIBS}
  DEFINES      ${STD_GEOMETRY_DEFINES}
)

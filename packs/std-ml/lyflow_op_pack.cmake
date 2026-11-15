file(GLOB STD_ML_SOURCES CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/ops/*.cpp")
file(GLOB STD_ML_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")

lyflow_op_pack(
  NAME         std-ml
  VERSION      0.3.0
  DEFAULT      ON
  SOURCES      ${STD_ML_SOURCES}
  TEST_SOURCES ${STD_ML_TESTS}
  LINK         lyflow_onnx_support
)

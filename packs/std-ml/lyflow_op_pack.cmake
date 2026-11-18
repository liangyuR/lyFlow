file(GLOB STD_ML_SOURCES CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/ops/*.cpp")
file(GLOB STD_ML_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")

set(STD_ML_LIBS lyflow_model_contracts)
set(STD_ML_DEFINES)
if(TARGET lyflow_ml_runtime)
  list(APPEND STD_ML_LIBS lyflow_ml_runtime)
  list(APPEND STD_ML_DEFINES LYFLOW_ML_RUNTIME=1)
else()
  list(REMOVE_ITEM STD_ML_SOURCES "${LYFLOW_PACK_DIR}/ops/ml_onnx_run.cpp")
  list(REMOVE_ITEM STD_ML_TESTS "${LYFLOW_PACK_DIR}/tests/test_ml_ops.cpp")
endif()

lyflow_op_pack(
  NAME         std-ml
  VERSION      0.4.0
  DEFAULT      ON
  SOURCES      ${STD_ML_SOURCES}
  TEST_SOURCES ${STD_ML_TESTS}
  LINK         ${STD_ML_LIBS}
  DEFINES      ${STD_ML_DEFINES}
)

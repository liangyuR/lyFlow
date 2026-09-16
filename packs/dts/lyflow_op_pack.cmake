# 车门胶条面差（dts-check）的算子包。零第三方依赖：直线/圆拟合、滑动中值、分面全是包内实现，
# 不链 PCL —— 产线上只需要带一个 lyflow_core.dll。
# 生产构建：-DLYFLOW_STD_PACKS=0 -DLYFLOW_OP_PACKS=<本目录>

file(GLOB DTS_SOURCES CONFIGURE_DEPENDS
  "${LYFLOW_PACK_DIR}/ops/*.cpp" "${LYFLOW_PACK_DIR}/algo/*.cpp")
file(GLOB DTS_TESTS CONFIGURE_DEPENDS "${LYFLOW_PACK_DIR}/tests/*.cpp")

lyflow_op_pack(
  NAME         dts
  VERSION      0.1.0
  DEFAULT      OFF
  SOURCES      ${DTS_SOURCES}
  TEST_SOURCES ${DTS_TESTS}
  INCLUDES     "${LYFLOW_PACK_DIR}"
)

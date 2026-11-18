set(_ml_modules model-contracts)
set(_ml_optional ml-runtime)
if(LYFLOW_STD_PACKS)
  list(APPEND _ml_modules ml-runtime)
  unset(_ml_optional)
endif()
lyflow_pack_dependencies(NAME std-ml DEFAULT ON
  MODULES ${_ml_modules} OPTIONAL_MODULES ${_ml_optional})

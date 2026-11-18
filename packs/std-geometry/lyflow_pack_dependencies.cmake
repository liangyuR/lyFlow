set(_geometry_modules geometry-kernel)
set(_geometry_optional workspace-io)
if(LYFLOW_STD_PACKS)
  list(APPEND _geometry_modules workspace-io)
  unset(_geometry_optional)
endif()
lyflow_pack_dependencies(NAME std-geometry DEFAULT ON
  MODULES ${_geometry_modules} OPTIONAL_MODULES ${_geometry_optional})

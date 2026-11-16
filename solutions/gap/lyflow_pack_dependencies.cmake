lyflow_pack_dependencies(NAME solution-gap DEFAULT OFF
  MODULES gap-geometry gap-model-adapters profile-contract metrology-kernel
  OPTIONAL_MODULES gap-template gap-model gap-import gap-compat
  PACKS std-profile std-metrology std-geometry REPLACES gap)

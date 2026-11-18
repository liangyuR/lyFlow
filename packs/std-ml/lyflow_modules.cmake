function(lyflow_setup_model_contracts)
  add_library(lyflow_model_contracts INTERFACE)
  target_include_directories(lyflow_model_contracts INTERFACE "${LYFLOW_PACK_ROOT}/std-ml/include")
  target_link_libraries(lyflow_model_contracts INTERFACE lyflow_data_contract)
endfunction()

lyflow_declare_module(NAME model-contracts TARGET lyflow_model_contracts SETUP lyflow_setup_model_contracts
  REQUIRES data-contract)

function(lyflow_setup_workspace_io)
  add_library(lyflow_workspace_io INTERFACE)
  target_link_libraries(lyflow_workspace_io INTERFACE lyflow_yaml_support)
endfunction()

lyflow_declare_module(NAME workspace-io TARGET lyflow_workspace_io SETUP lyflow_setup_workspace_io
  REQUIRES yaml-support)

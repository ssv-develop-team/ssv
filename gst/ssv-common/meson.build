ssv_common_inc = include_directories('include')

ssv_common_lib = shared_library('ssv_common',
  'ssv_config.cpp',
  'ssv_meta.cpp',
  include_directories : ssv_common_inc,
  dependencies : [gst_deps, yaml_dep],
  install : true,
)

ssv_common_dep = declare_dependency(
  link_with : ssv_common_lib,
  include_directories : ssv_common_inc,
  dependencies : [gst_deps, yaml_dep],
)

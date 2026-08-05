ssv_overlay_inc = include_directories('.')

ssv_overlay_core_lib = static_library('ssv-overlay-core',
  'overlay_motion_predictor.cpp',
  'overlay_presentation.cpp',
  include_directories : ssv_overlay_inc,
  dependencies : [gst_deps, ssv_common_dep],
  pic : true,
)

ssv_overlay_core_dep = declare_dependency(
  link_with : ssv_overlay_core_lib,
  include_directories : ssv_overlay_inc,
  dependencies : [gst_deps, ssv_common_dep],
)

ssv_overlay_lib = shared_library('gstssvoverlay',
  'gstssvoverlay.cpp',
  'overlay_renderer.cpp',
  'overlay_runtime.cpp',
  include_directories : ssv_overlay_inc,
  dependencies : [gst_deps, gst_base_dep, gst_video_dep, ssv_common_dep],
  link_with : ssv_overlay_core_lib,
  cpp_args : ['-DPACKAGE="gstssvoverlay"'],
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0',
)

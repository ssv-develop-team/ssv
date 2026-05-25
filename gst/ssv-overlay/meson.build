shared_library('gstssvoverlay',
  'gstssvoverlay.cpp',
  dependencies : [gst_deps, gst_base_dep, gst_video_dep, ssv_common_dep],
  cpp_args : ['-DPACKAGE="gstssvoverlay"'],
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0',
)

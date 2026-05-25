shared_library('gstssvtrack',
  'gstssvtrack.cpp',
  dependencies : [gst_deps, gst_base_dep, gst_video_dep, ssv_common_dep],
  cpp_args : ['-DPACKAGE="gstssvtrack"'],
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0',
)

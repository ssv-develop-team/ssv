shared_library('gstssvpub',
  'gstssvpub.cpp',
  dependencies : [gst_deps, gst_base_dep, gst_video_dep, ssv_common_dep,
                  hiredis_dep, json_dep],
  cpp_args : ['-DPACKAGE="gstssvpub"'],
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0',
)

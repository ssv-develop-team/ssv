shared_library('gstssvinfer',
  'gstssvinfer.cpp',
  dependencies : [gst_deps, gst_base_dep, gst_video_dep, ssv_common_dep, onnx_dep],
  cpp_args : ['-DPACKAGE="gstssvinfer"'],
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0',
)

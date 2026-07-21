ssv_infer_sources = [
  'gstssvinfer.cpp',
  'ssv_backend_factory.cpp',
  'ssv_inference_config.cpp',
  'ssv_inference_engine.cpp',
  'ssv_onnxruntime_backend.cpp',
  'ssv_preprocessor.cpp',
  'ssv_yolo_parser.cpp',
]
ssv_infer_deps = [gst_deps, gst_base_dep, gst_video_dep, ssv_common_dep, onnx_dep]

if tensorrt_mode != 'disabled'
  if tensorrt_enabled
    message('Building TensorRT inference backend')
    ssv_infer_sources += ['ssv_tensorrt_backend.cpp']
    ssv_infer_deps += [tensorrt_dep]
  elif tensorrt_mode == 'enabled'
    error('TensorRT SDK not found; install TensorRT headers/libs or build with tensorrt_mode=disabled')
  else
    message('TensorRT SDK not found; building TensorRT unavailable stub')
    ssv_infer_sources += ['ssv_tensorrt_backend_stub.cpp']
  endif
else
  ssv_infer_sources += ['ssv_tensorrt_backend_stub.cpp']
endif

shared_library('gstssvinfer',
  ssv_infer_sources,
  dependencies : ssv_infer_deps,
  cpp_args : ['-DPACKAGE="gstssvinfer"'],
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0',
)

ssv_inference_core_sources = [
  'core/ssv_backend_factory.cpp',
  'core/ssv_inference_buffer.cpp',
  'core/ssv_inference_config.cpp',
  'core/ssv_inference_engine.cpp',
  'core/ssv_inference_service.cpp',
  'core/ssv_inference_stats.cpp',
  'core/ssv_latest_frame_scheduler.cpp',
  'model/ssv_image_preprocessor.cpp',
  'model/ssv_model_contract.cpp',
  'model/ssv_yolo_parser.cpp',
  'backends/onnxruntime/ssv_onnxruntime_backend.cpp',
  'backends/onnxruntime/ssv_provider_resolver.cpp',
  'backends/onnxruntime/ssv_session_pool.cpp',
  'backends/tensorrt/ssv_tensorrt_manifest.cpp',
]
ssv_inference_public_inc = include_directories('public')
ssv_inference_private_inc = include_directories('.')

dynamic_loader_dep = meson.get_compiler('cpp').find_library('dl')
ssv_inference_core_deps = [
  gst_deps,
  ssv_common_dep,
  onnx_dep,
  json_dep,
  dynamic_loader_dep,
]

if tensorrt_mode != 'disabled'
  if tensorrt_enabled
    message('Building TensorRT inference backend')
    ssv_inference_core_sources += [
      'backends/tensorrt/ssv_tensorrt_backend.cpp',
      'backends/tensorrt/ssv_tensorrt_resources.cpp',
    ]
    if cuda_enabled
      ssv_inference_core_sources += [
        'backends/tensorrt/ssv_tensorrt_cuda_kernel.cu',
      ]
    else
      ssv_inference_core_sources += [
        'backends/tensorrt/ssv_tensorrt_cuda_kernel.cpp',
      ]
    endif
    ssv_inference_core_deps += [tensorrt_dep]
  elif tensorrt_mode == 'enabled'
    error('TensorRT SDK not found; install TensorRT headers/libs or build with tensorrt_mode=disabled')
  else
    message('TensorRT SDK not found; building TensorRT unavailable stub')
    ssv_inference_core_sources += [
      'backends/tensorrt/ssv_tensorrt_backend_stub.cpp',
    ]
  endif
else
  ssv_inference_core_sources += [
    'backends/tensorrt/ssv_tensorrt_backend_stub.cpp',
  ]
endif

ssv_inference_core_lib = shared_library('ssv-inference-core',
  ssv_inference_core_sources,
  include_directories : [ssv_inference_public_inc, ssv_inference_private_inc],
  dependencies : ssv_inference_core_deps,
  cpp_args : [
    '-DSSV_ONNXRUNTIME_PROFILE="@0@"'.format(onnxruntime_profile),
    '-DSSV_ONNXRUNTIME_DEPENDENCY_SIGNATURE="@0@"'.format(
      onnxruntime_dependency_signature),
  ],
  install : true,
)

ssv_inference_core_dep = declare_dependency(
  link_with : ssv_inference_core_lib,
  include_directories : ssv_inference_public_inc,
  dependencies : [gst_deps, ssv_common_dep],
)

subdir('tests')

ssv_infer_lib = shared_library('gstssvinfer',
  'plugin/gstssvinfer.cpp',
  dependencies : [gst_deps, gst_base_dep, gst_video_dep, ssv_inference_core_dep],
  cpp_args : ['-DPACKAGE="gstssvinfer"'],
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0',
)

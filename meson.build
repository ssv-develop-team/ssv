project('site-safety-vision', 'cpp',
  version : '0.1.0',
  default_options : ['cpp_std=c++20', 'warning_level=2'],
  meson_version : '>= 1.1',
)

fs = import('fs')
cpp = meson.get_compiler('cpp')

gst_deps = dependency('gstreamer-1.0')
gst_base_dep = dependency('gstreamer-base-1.0')
gst_app_dep = dependency('gstreamer-app-1.0')
gst_video_dep = dependency('gstreamer-video-1.0')
gst_check_dep = dependency('gstreamer-check-1.0')
yaml_dep = dependency('yaml-cpp')
hiredis_dep = dependency('hiredis', required : false)
json_dep = dependency('nlohmann_json', required : false)

tensorrt_opt = get_option('tensorrt')
tensorrt_dep = dependency('nvinfer', required : false)
if not tensorrt_dep.found()
  tensorrt_header = fs.exists('/usr/include/NvInfer.h') or fs.exists('/usr/local/cuda/include/NvInfer.h')
  tensorrt_lib = cpp.find_library('nvinfer', required : false)
  cudart_lib = cpp.find_library('cudart', required : false)
  if tensorrt_header and tensorrt_lib.found() and cudart_lib.found()
    tensorrt_dep = declare_dependency(
      compile_args : fs.exists('/usr/local/cuda/include/NvInfer.h') ? ['-I/usr/local/cuda/include'] : [],
      dependencies : [tensorrt_lib, cudart_lib],
    )
  endif
endif
if tensorrt_opt == 'enabled' and not tensorrt_dep.found()
  error('TensorRT SDK not found, but -Dtensorrt=enabled was requested')
endif
if tensorrt_dep.found()
  message('TensorRT backend enabled')
else
  message('TensorRT backend disabled; using explicit unavailable backend')
endif

# ONNX Runtime — try pkg-config first, then cmake
onnx_dep = dependency('onnxruntime', required : false)
if not onnx_dep.found()
  onnx_dep = dependency('onnxruntime', method : 'cmake', required : false)
endif
if not onnx_dep.found()
  onnx_local_root = meson.project_source_root() / '.deps' / 'onnxruntime'
  onnx_local_include = onnx_local_root / 'include'
  onnx_local_lib = onnx_local_root / 'lib' / 'libonnxruntime.so'
  if fs.exists(onnx_local_include / 'onnxruntime_cxx_api.h') and fs.exists(onnx_local_lib)
    message('Using local ONNX Runtime from ' + onnx_local_root)
    onnx_dep = declare_dependency(
      compile_args : ['-I' + onnx_local_include],
      link_args : ['-L' + (onnx_local_root / 'lib'), '-lonnxruntime'],
    )
  endif
endif

conf_data = configuration_data()
conf_data.set_quoted('SSV_VERSION', meson.project_version())
conf_data.set_quoted('SSV_PACKAGE_NAME', 'site-safety-vision')

subdir('gst')

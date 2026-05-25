project('site-safety-vision', 'cpp',
  version : '0.1.0',
  default_options : ['cpp_std=c++20', 'warning_level=2'],
  meson_version : '>= 1.1',
)

fs = import('fs')

gst_deps = dependency('gstreamer-1.0')
gst_base_dep = dependency('gstreamer-base-1.0')
gst_app_dep = dependency('gstreamer-app-1.0')
gst_video_dep = dependency('gstreamer-video-1.0')
gst_check_dep = dependency('gstreamer-check-1.0')
yaml_dep = dependency('yaml-cpp')
hiredis_dep = dependency('hiredis', required : false)
json_dep = dependency('nlohmann_json', required : false)

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

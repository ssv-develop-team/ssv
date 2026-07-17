gst_test_deps = [gst_deps, gst_base_dep, gst_app_dep, gst_video_dep, gst_check_dep, ssv_common_dep, onnx_dep]
if opencv_enabled
  gst_test_deps += opencv_dep
  opencv_libdir = opencv_dep.get_variable(pkgconfig : 'libdir')
endif

gst_unit_tests = executable('gst-unit-tests',
  'test_ssv_meta.cpp',
  'test_gst_plugins.cpp',
  '../ssv-infer/ssv_inference_config.cpp',
  '../ssv-infer/ssv_preprocessor.cpp',
  '../ssv-infer/ssv_yolo_parser.cpp',
  '../ssv-track/botsort/botsort_kalman.cpp',
  '../ssv-track/botsort/botsort_matching.cpp',
  '../ssv-track/botsort/botsort_lapjv.cpp',
  '../ssv-track/botsort/botsort_gmc.cpp',
  '../ssv-track/botsort/botsort_tracker.cpp',
  '../ssv-track/botsort/botsort_processor.cpp',
  include_directories : [include_directories('../ssv-infer')],
  dependencies : gst_test_deps,
  cpp_args : ['-DSSV_HAS_OPENCV=' + (opencv_enabled ? '1' : '0')],
  link_with : [ssv_pub_lib, ssv_overlay_lib],
)

test('gst-unit-tests', gst_unit_tests,
  env : {
    'GST_REGISTRY' : meson.project_build_root() / 'gst-test-registry.bin',
    'GST_PLUGIN_PATH' : meson.project_build_root() / 'gst' / 'ssv-template'
      + ':' + meson.project_build_root() / 'gst' / 'ssv-infer'
      + ':' + meson.project_build_root() / 'gst' / 'ssv-track'
      + ':' + meson.project_build_root() / 'gst' / 'ssv-pub'
      + ':' + meson.project_build_root() / 'gst' / 'ssv-overlay',
    'LD_LIBRARY_PATH' : (opencv_enabled ? opencv_libdir + ':' : '')
      + meson.project_build_root() / 'gst' / 'ssv-common'
      + ':' + meson.project_source_root() / '.deps' / 'onnxruntime' / 'lib'
      + ':' + meson.project_source_root() / '.deps' / 'tensorrt' / 'TensorRT-11.1.0.106' / 'lib',
  },
)

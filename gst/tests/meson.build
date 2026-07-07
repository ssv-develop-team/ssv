gst_unit_tests = executable('gst-unit-tests',
  'test_ssv_meta.cpp',
  'test_gst_plugins.cpp',
  '../ssv-infer/ssv_inference_config.cpp',
  '../ssv-infer/ssv_preprocessor.cpp',
  '../ssv-infer/ssv_yolo_parser.cpp',
  include_directories : [include_directories('../ssv-infer')],
  dependencies : [gst_deps, gst_base_dep, gst_app_dep, gst_video_dep, gst_check_dep, ssv_common_dep],
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
    'LD_LIBRARY_PATH' : meson.project_build_root() / 'gst' / 'ssv-common'
      + ':' + meson.project_source_root() / '.deps' / 'onnxruntime' / 'lib',
  },
)

gst_unit_tests = executable('gst-unit-tests',
  'test_ssv_meta.cpp',
  'test_gst_plugins.cpp',
  dependencies : [gst_deps, gst_base_dep, gst_app_dep, gst_video_dep, gst_check_dep, ssv_common_dep],
)

test('gst-unit-tests', gst_unit_tests,
  env : {
    'GST_PLUGIN_PATH' : meson.project_build_root() / 'gst' / 'ssv-template'
      + ':' + meson.project_build_root() / 'gst' / 'ssv-infer'
      + ':' + meson.project_build_root() / 'gst' / 'ssv-track'
      + ':' + meson.project_build_root() / 'gst' / 'ssv-pub'
      + ':' + meson.project_build_root() / 'gst' / 'ssv-overlay',
    'LD_LIBRARY_PATH' : meson.project_build_root() / 'gst' / 'ssv-common',
  },
)

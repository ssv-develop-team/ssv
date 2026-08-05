ssv_display_window_test = executable('ssv-display-window-test',
  'test_ssv_display_window.cpp',
  include_directories : runner_inc,
  dependencies : [ssv_runner_display_dep],
)

test('ssv-display-window', ssv_display_window_test,
  env : {
    'GST_REGISTRY' : meson.project_build_root() / 'gst-display-window-test-registry.bin',
  },
  suite : 'display',
)

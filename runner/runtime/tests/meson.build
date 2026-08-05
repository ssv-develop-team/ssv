ssv_run_attempt_test = executable('ssv-run-attempt-test',
  'test_ssv_run_attempt.cpp',
  include_directories : runner_inc,
  dependencies : [
    gst_video_dep,
    ssv_runner_runtime_dep,
    ssv_inference_test_support_dep,
  ],
)

ssv_runner_test = executable('ssv-runner-test',
  'test_ssv_runner.cpp',
  include_directories : runner_inc,
  dependencies : [ssv_runner_runtime_dep],
)

ssv_runtime_event_adapter_test = executable('ssv-runtime-event-adapter-test',
  'test_ssv_runtime_event_adapter.cpp',
  include_directories : runner_inc,
  dependencies : [ssv_runner_runtime_dep],
)

test('ssv-run-attempt', ssv_run_attempt_test,
  env : {
    'GST_REGISTRY' : meson.project_build_root() / 'gst-run-attempt-test-registry.bin',
    'SSV_TEST_LABEL_MAP_PATH' : meson.project_source_root() / 'config/model-labels/coco80.txt',
  },
  suite : 'runtime',
)
test('ssv-runner', ssv_runner_test,
  suite : 'runtime',
)
test('ssv-runtime-event-adapter', ssv_runtime_event_adapter_test,
  suite : ['observability', 'runtime'],
)

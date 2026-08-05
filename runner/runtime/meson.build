ssv_runner_runtime_lib = static_library('ssv-runner-runtime',
  'ssv_runtime_event_adapter.cpp',
  'ssv_run_attempt.cpp',
  'ssv_run_attempt_factory.cpp',
  'ssv_run_fallback_state.cpp',
  'ssv_runner.cpp',
  include_directories : runner_inc,
  dependencies : [
    gst_deps,
    gst_gl_dep,
    gst_video_dep,
    ssv_common_dep,
    ssv_inference_core_dep,
    ssv_runner_observability_dep,
    ssv_runner_display_dep,
    ssv_runner_pipeline_dep,
  ],
  cpp_args : runner_feature_args,
)

ssv_runner_runtime_dep = declare_dependency(
  link_with : ssv_runner_runtime_lib,
  include_directories : runner_inc,
  dependencies : [
    gst_deps,
    gst_gl_dep,
    gst_video_dep,
    ssv_common_dep,
    ssv_inference_core_dep,
    ssv_runner_observability_dep,
    ssv_runner_display_dep,
    ssv_runner_pipeline_dep,
  ],
)

subdir('tests')

ssv_runner_observability_lib = static_library('ssv-runner-observability',
  'ssv_event_log.cpp',
  'ssv_stderr_log_sink.cpp',
  include_directories : runner_inc,
)

ssv_runner_observability_dep = declare_dependency(
  link_with : ssv_runner_observability_lib,
  include_directories : runner_inc,
)

subdir('tests')

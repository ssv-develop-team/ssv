ssv_event_log_test = executable('ssv-event-log-test',
  'test_ssv_event_log.cpp',
  include_directories : runner_inc,
  dependencies : [ssv_runner_observability_dep],
)

ssv_stderr_log_sink_test = executable('ssv-stderr-log-sink-test',
  'test_ssv_stderr_log_sink.cpp',
  include_directories : runner_inc,
  dependencies : [ssv_runner_observability_dep],
  link_args : ['-Wl,--wrap=write'],
)

test('ssv-event-log', ssv_event_log_test,
  suite : 'observability',
)
test('ssv-stderr-log-sink', ssv_stderr_log_sink_test,
  suite : 'observability',
)

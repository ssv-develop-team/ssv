ssv_track_test_deps = [
  gst_deps,
  gst_base_dep,
  gst_video_dep,
  gst_check_dep,
  ssv_common_dep,
]
if opencv_enabled
  ssv_track_test_deps += opencv_dep
endif

ssv_track_core_sources = [
  '../core/botsort/botsort_kalman.cpp',
  '../core/botsort/botsort_matching.cpp',
  '../core/botsort/botsort_lapjv.cpp',
  '../core/botsort/botsort_gmc.cpp',
  '../core/botsort/botsort_tracker.cpp',
]

ssv_track_processor_contract_test = executable(
  'ssv-botsort-processor-contract-test',
  'test_botsort_processor_contract.cpp',
  ssv_track_core_sources,
  '../adapters/ssv_track_adapter.cpp',
  include_directories : ssv_track_private_inc,
  dependencies : ssv_track_test_deps,
  cpp_args : ['-DSSV_HAS_OPENCV=' + (opencv_enabled ? '1' : '0')],
)

ssv_track_tracker_test = executable(
  'ssv-botsort-tracker-test',
  'test_botsort_tracker.cpp',
  ssv_track_core_sources,
  include_directories : ssv_track_private_inc,
  dependencies : (opencv_enabled ? [opencv_dep] : []),
  cpp_args : ['-DSSV_HAS_OPENCV=' + (opencv_enabled ? '1' : '0')],
)

test('ssv-botsort-processor-contract', ssv_track_processor_contract_test)
test('ssv-botsort-tracker', ssv_track_tracker_test)

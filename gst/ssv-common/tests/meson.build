ssv_common_meta_types_test = executable('ssv-meta-types-test',
  'test_ssv_meta_types.cpp',
  dependencies : [gst_deps, ssv_common_dep],
)

ssv_common_meta_timeline_test = executable('ssv-meta-timeline-test',
  'test_ssv_meta_timeline.cpp',
  dependencies : [gst_deps, ssv_common_dep],
)

ssv_common_meta_test = executable('ssv-meta-test',
  'test_ssv_meta.cpp',
  dependencies : [gst_deps, ssv_common_dep],
)

ssv_common_analysis_frame_test = executable('ssv-analysis-frame-test',
  'test_ssv_analysis_frame.cpp',
  dependencies : [gst_deps, gst_video_dep, ssv_common_dep],
)

ssv_common_config_test = executable('ssv-config-test',
  'test_ssv_config.cpp',
  dependencies : [ssv_common_dep],
)

test('ssv-meta-types', ssv_common_meta_types_test)
test('ssv-meta-timeline', ssv_common_meta_timeline_test)
test('ssv-meta', ssv_common_meta_test)
test('ssv-analysis-frame', ssv_common_analysis_frame_test)
test('ssv-config', ssv_common_config_test,
  args : [meson.project_source_root() / 'config/ssv.example.yaml'],
)

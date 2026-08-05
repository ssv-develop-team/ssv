gst_test_deps = [gst_deps, gst_base_dep, gst_app_dep, gst_video_dep, gst_check_dep, ssv_common_dep]
test_label_map_path = meson.project_source_root() / 'config/model-labels/coco80.txt'
if opencv_enabled
  gst_test_deps += opencv_dep
endif

gst_unit_tests = executable('gst-unit-tests',
  'test_overlay_renderer_contract.cpp',
  'test_gst_plugins.cpp',
  include_directories : [
    ssv_inference_public_inc,
    include_directories('../ssv-overlay'),
  ],
  dependencies : gst_test_deps + [ssv_inference_test_support_dep],
  cpp_args : ['-DSSV_HAS_OPENCV=' + (opencv_enabled ? '1' : '0')],
  link_with : [ssv_inference_core_lib, ssv_pub_lib, ssv_overlay_lib],
)

ssv_pub_payload_test = executable('ssv-pub-payload-test',
  'test_ssv_pub_payload.cpp',
  include_directories : [include_directories('../ssv-pub')],
  dependencies : [gst_deps, ssv_common_dep, json_dep],
  link_with : [ssv_pub_lib],
)

ssv_overlay_motion_predictor_test = executable('ssv-overlay-motion-predictor-test',
  'test_overlay_motion_predictor.cpp',
  include_directories : [include_directories('../ssv-overlay')],
  dependencies : [ssv_overlay_core_dep],
)

ssv_overlay_presentation_test = executable('ssv-overlay-presentation-test',
  'test_overlay_presentation.cpp',
  include_directories : [include_directories('../ssv-overlay')],
  dependencies : [ssv_overlay_core_dep],
)

ssv_overlay_runtime_test = executable('ssv-overlay-runtime-test',
  'test_overlay_runtime.cpp',
  '../ssv-overlay/overlay_renderer.cpp',
  '../ssv-overlay/overlay_runtime.cpp',
  include_directories : [include_directories('../ssv-overlay')],
  dependencies : [gst_video_dep, ssv_overlay_core_dep],
)

test('ssv-pub-payload', ssv_pub_payload_test)
test('ssv-overlay-motion-predictor', ssv_overlay_motion_predictor_test)
test('ssv-overlay-presentation', ssv_overlay_presentation_test)
test('ssv-overlay-runtime', ssv_overlay_runtime_test)

test('gst-unit-tests', gst_unit_tests,
  env : {
    'GST_REGISTRY' : meson.project_build_root() / 'gst-test-registry.bin',
    'GST_PLUGIN_PATH' : ssv_gst_test_plugin_path,
    'LD_LIBRARY_PATH' : ssv_gst_test_runtime_library_path,
    'SSV_TEST_LABEL_MAP_PATH' : test_label_map_path,
  },
)

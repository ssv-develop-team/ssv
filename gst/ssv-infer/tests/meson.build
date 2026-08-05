ssv_inference_test_label_map_path = meson.project_source_root() / 'config/model-labels/coco80.txt'

ssv_inference_test_support_lib = static_library('ssv-inference-test-support',
  'support/ssv_inference_test_service.cpp',
  include_directories : [
    ssv_inference_private_inc,
    include_directories('support'),
  ],
  dependencies : [ssv_inference_core_dep],
)

ssv_inference_public_interface_test = executable(
  'ssv-inference-public-interface-test',
  'test_ssv_inference_public_interface.cpp',
  dependencies : [ssv_inference_core_dep],
)

test(
  'ssv-inference-public-interface',
  ssv_inference_public_interface_test,
)

ssv_inference_test_support_dep = declare_dependency(
  link_with : ssv_inference_test_support_lib,
  include_directories : include_directories('support'),
  dependencies : [ssv_inference_core_dep],
)

ssv_inference_service_test = executable('ssv-inference-service-test',
  'test_ssv_inference_service.cpp',
  include_directories : [ssv_inference_public_inc, ssv_inference_private_inc],
  dependencies : [gst_deps, ssv_common_dep],
  link_with : [ssv_inference_core_lib],
)

ssv_latest_frame_scheduler_test = executable(
  'ssv-latest-frame-scheduler-test',
  'test_ssv_latest_frame_scheduler.cpp',
  include_directories : [ssv_inference_public_inc, ssv_inference_private_inc],
  dependencies : [gst_deps, ssv_common_dep],
  link_with : [ssv_inference_core_lib],
)

ssv_yolo_parser_test = executable('ssv-yolo-parser-test',
  'test_ssv_yolo_parser.cpp',
  include_directories : [ssv_inference_public_inc, ssv_inference_private_inc],
  dependencies : [gst_deps, ssv_common_dep],
  link_with : [ssv_inference_core_lib],
)

ssv_provider_runtime_test = executable('ssv-provider-runtime-test',
  'test_ssv_provider_runtime.cpp',
  include_directories : [ssv_inference_public_inc, ssv_inference_private_inc],
  dependencies : [gst_deps, ssv_common_dep],
  link_with : [ssv_inference_core_lib],
)

ssv_tensorrt_manifest_test = executable('ssv-tensorrt-manifest-test',
  'test_ssv_tensorrt_manifest.cpp',
  include_directories : [ssv_inference_public_inc, ssv_inference_private_inc],
  dependencies : [gst_deps, ssv_common_dep, json_dep],
  link_with : [ssv_inference_core_lib],
)

if tensorrt_enabled
  ssv_tensorrt_resources_test = executable('ssv-tensorrt-resources-test',
    'test_ssv_tensorrt_resources.cpp',
    include_directories : [ssv_inference_public_inc, ssv_inference_private_inc],
    dependencies : [ssv_common_dep],
    link_with : [ssv_inference_core_lib],
  )
endif

ssv_tensorrt_unavailable_test = executable('ssv-tensorrt-unavailable-test',
  'test_ssv_tensorrt_unavailable.cpp',
  '../backends/tensorrt/ssv_tensorrt_backend_stub.cpp',
  include_directories : [ssv_inference_public_inc, ssv_inference_private_inc],
  dependencies : [gst_deps, ssv_common_dep],
)

ssv_inference_plugin_test_deps = [
  gst_deps,
  gst_base_dep,
  gst_app_dep,
  gst_video_dep,
  gst_check_dep,
  ssv_common_dep,
]

ssv_infer_plugin_test = executable('ssv-infer-plugin-test',
  'test_gstssvinfer.cpp',
  include_directories : [ssv_inference_public_inc, include_directories('support')],
  dependencies : ssv_inference_plugin_test_deps + [ssv_inference_test_support_dep],
)

ssv_infer_plugin_test_env = {
  'GST_REGISTRY' : meson.project_build_root() / 'gst-infer-plugin-test-registry.bin',
  'GST_PLUGIN_PATH' : ssv_gst_test_plugin_path,
  'LD_LIBRARY_PATH' : ssv_gst_test_runtime_library_path,
  'SSV_TEST_LABEL_MAP_PATH' : ssv_inference_test_label_map_path,
}

test('ssv-inference-service', ssv_inference_service_test,
  env : {
    'GST_REGISTRY' : meson.project_build_root() / 'gst-inference-service-test-registry.bin',
    'SSV_TEST_LABEL_MAP_PATH' : ssv_inference_test_label_map_path,
  },
)
test('ssv-latest-frame-scheduler', ssv_latest_frame_scheduler_test)
test('ssv-yolo-parser', ssv_yolo_parser_test)
test('ssv-provider-runtime', ssv_provider_runtime_test,
  env : {
    'GST_REGISTRY' : meson.project_build_root() / 'gst-provider-runtime-test-registry.bin',
    'SSV_TEST_LABEL_MAP_PATH' : ssv_inference_test_label_map_path,
  },
)
test('ssv-tensorrt-manifest', ssv_tensorrt_manifest_test)
if tensorrt_enabled
  test('ssv-tensorrt-resources', ssv_tensorrt_resources_test)
endif
test('ssv-tensorrt-unavailable', ssv_tensorrt_unavailable_test)
test('ssv-infer-plugin', ssv_infer_plugin_test, env : ssv_infer_plugin_test_env)

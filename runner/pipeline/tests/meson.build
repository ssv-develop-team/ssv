ssv_pipeline_plan_test = executable('ssv-pipeline-plan-test',
  'test_ssv_pipeline_plan.cpp',
  include_directories : runner_inc,
  dependencies : [ssv_runner_pipeline_dep],
)

ssv_pipeline_contract_test = executable('ssv-pipeline-contract-test',
  'test_ssv_pipeline_contract.cpp',
  include_directories : runner_inc,
  dependencies : [gst_app_dep, ssv_runner_pipeline_dep],
)

ssv_pipeline_builder_test = executable('ssv-pipeline-builder-test',
  'test_ssv_pipeline_builder.cpp',
  include_directories : runner_inc,
  dependencies : [ssv_runner_pipeline_dep, ssv_inference_test_support_dep],
)

test('ssv-pipeline-plan', ssv_pipeline_plan_test,
  suite : 'pipeline',
)
test('ssv-pipeline-contract', ssv_pipeline_contract_test,
  env : {
    'GST_REGISTRY' : meson.project_build_root() / 'gst-pipeline-contract-test-registry.bin',
  },
  suite : 'pipeline',
)
test('ssv-pipeline-builder', ssv_pipeline_builder_test,
  suite : 'pipeline',
  env : {
    'GST_REGISTRY' : meson.project_build_root() / 'gst-pipeline-builder-test-registry.bin',
    'GST_PLUGIN_PATH' : ssv_gst_test_plugin_path,
    'LD_LIBRARY_PATH' : ssv_gst_test_runtime_library_path,
    'SSV_TEST_LABEL_MAP_PATH' : ssv_inference_test_label_map_path,
  },
)

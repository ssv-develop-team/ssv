ssv_runner_pipeline_lib = static_library('ssv-runner-pipeline',
  'ssv_hardware_capabilities.cpp',
  'ssv_pipeline_builder.cpp',
  'ssv_pipeline_contract.cpp',
  'ssv_pipeline_instance.cpp',
  'ssv_pipeline_plan.cpp',
  include_directories : runner_inc,
  dependencies : [
    gst_deps,
    gst_video_dep,
    ssv_common_dep,
    ssv_inference_core_dep,
  ],
  cpp_args : runner_feature_args,
)

ssv_runner_pipeline_dep = declare_dependency(
  link_with : ssv_runner_pipeline_lib,
  include_directories : runner_inc,
  dependencies : [
    gst_deps,
    gst_video_dep,
    ssv_common_dep,
    ssv_inference_core_dep,
  ],
)

subdir('tests')

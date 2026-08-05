ssv_runner_display_lib = static_library('ssv-runner-display',
  'ssv_display_window.cpp',
  include_directories : runner_inc,
  dependencies : [
    gst_deps,
    gst_video_dep,
    gtk_dep,
    ssv_common_dep,
    ssv_overlay_core_dep,
    ssv_runner_pipeline_dep,
  ],
)

ssv_runner_display_dep = declare_dependency(
  link_with : ssv_runner_display_lib,
  include_directories : runner_inc,
  dependencies : [
    gst_deps,
    gst_video_dep,
    gtk_dep,
    ssv_common_dep,
    ssv_overlay_core_dep,
    ssv_runner_pipeline_dep,
  ],
)

subdir('tests')

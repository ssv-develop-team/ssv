runner_inc = include_directories('.')

runner_feature_args = [
  '-DSSV_HAS_ONNXRUNTIME=1',
  '-DSSV_HAS_TENSORRT_ENGINE=' + (tensorrt_enabled ? '1' : '0'),
]

subdir('observability')
subdir('pipeline')
subdir('display')
subdir('runtime')

ssv_runner_exe = executable('ssv-runner',
  'main.cpp',
  dependencies : [
    gst_video_dep,
    gst_gl_dep,
    gtk_dep,
    yaml_dep,
    ssv_runner_runtime_dep,
  ],
  install : true,
)

subdir('tests')

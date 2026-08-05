test('ssv-runner-cli-model-error', find_program('bash'),
  args : [
    files('test_ssv_runner_cli.sh'),
    ssv_runner_exe,
    meson.project_source_root() / 'config/model-labels/coco80.txt',
  ],
  env : {
    'GST_REGISTRY' : meson.project_build_root() / 'gst-runner-cli-test-registry.bin',
  },
)

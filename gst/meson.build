ssv_gst_test_plugin_path = ':'.join([
  meson.project_build_root() / 'gst' / 'ssv-template',
  meson.project_build_root() / 'gst' / 'ssv-infer',
  meson.project_build_root() / 'gst' / 'ssv-track',
  meson.project_build_root() / 'gst' / 'ssv-pub',
  meson.project_build_root() / 'gst' / 'ssv-overlay',
])
ssv_gst_test_runtime_library_path = (deps_runtime_path == '' ? '' : deps_runtime_path + ':') + meson.project_build_root() / 'gst' / 'ssv-common'

subdir('ssv-common')
subdir('ssv-template')
subdir('ssv-infer')
subdir('ssv-track')
subdir('ssv-pub')
subdir('ssv-overlay')
subdir('tests')

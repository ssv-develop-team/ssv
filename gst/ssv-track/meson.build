ssv_track_deps = [gst_deps, gst_base_dep, gst_video_dep, ssv_common_dep]
ssv_track_cpp_args = ['-DPACKAGE="gstssvtrack"', '-DSSV_HAS_OPENCV=' + (opencv_enabled ? '1' : '0')]
ssv_track_private_inc = include_directories('.', 'core/botsort', 'adapters')
if opencv_enabled
  ssv_track_deps += opencv_dep
endif

ssv_track_lib = shared_library('gstssvtrack',
  'plugin/gstssvtrack.cpp',
  'core/botsort/botsort_kalman.cpp',
  'core/botsort/botsort_matching.cpp',
  'core/botsort/botsort_lapjv.cpp',
  'core/botsort/botsort_gmc.cpp',
  'core/botsort/botsort_tracker.cpp',
  'adapters/ssv_track_adapter.cpp',
  include_directories : ssv_track_private_inc,
  dependencies : ssv_track_deps,
  cpp_args : ssv_track_cpp_args,
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0',
)

subdir('tests')

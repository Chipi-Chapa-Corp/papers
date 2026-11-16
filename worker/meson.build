# The worker reuses the native print operation, which depends on libview.
document_worker = executable(
  'papers-document-worker',
  document_worker_sources,
  include_directories: [top_inc, libdocument_inc],
  dependencies: libppsview_dep,
  c_args: document_worker_cflags,
  install: true,
  install_dir: get_option('libexecdir'),
)

worker_env = environment()
worker_env.set('PPS_DOCUMENT_WORKER', document_worker.full_path())
backend_env.set('PPS_DOCUMENT_WORKER', document_worker.full_path())
meson.add_devenv(worker_env)

# Scanning has its own process so a blocked driver cannot freeze document tabs.
sane_dep = dependency('sane-backends', required: get_option('scanning'))
if sane_dep.found()
  scan_worker = executable(
    'dokument-scan-worker',
    'dokument-scan-worker.c',
    dependencies: [gio_dep, gdk_pixbuf_dep, sane_dep],
    install: true,
    install_dir: get_option('libexecdir'),
  )
  scan_env = environment()
  scan_env.set('DOKUMENT_SCAN_WORKER', scan_worker.full_path())
  meson.add_devenv(scan_env)
  if get_option('tests')
    scan_test_backend = shared_library('scan-test-backend', 'scan-test-backend.c',
      dependencies: [sane_dep, cc.find_library('dl')],
      install: false,
    )
    test('scan-worker', find_program('python3'),
      args: [files('test-scan-worker.py'), scan_worker, scan_test_backend],
      timeout: 60,
    )
  endif
endif
config_h.set_quoted('SCAN_WORKER', pps_prefix / get_option('libexecdir') / 'dokument-scan-worker')

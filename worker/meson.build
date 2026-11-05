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

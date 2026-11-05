test_worker = executable(
  'test-pps-worker',
  'test-pps-worker.c',
  document_dbus[1],
  include_directories: top_inc,
  dependencies: libppsdocument_dep,
  c_args: [
    '-DPAPERS_COMPILATION',
    '-DTESTDATADIR="@0@"'.format(join_paths(source_root, 'test-data')),
  ],
)

test('pps-worker-test', test_worker, env: backend_env, depends: document_worker)

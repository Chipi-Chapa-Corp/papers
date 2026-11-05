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

foreach name: ['buffer', 'info']
  test_executable = executable(
    'test-pps-' + name,
    'test-pps-@0@.c'.format(name),
    include_directories: top_inc,
    dependencies: libppsdocument_dep,
    c_args: '-DPAPERS_COMPILATION',
  )
  test('pps-@0@-test'.format(name), test_executable)
endforeach

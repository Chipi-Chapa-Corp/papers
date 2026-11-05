foreach name: ['worker', 'proxy']
  test_executable = executable(
    'test-pps-' + name,
    'test-pps-@0@.c'.format(name),
    document_dbus[1],
    include_directories: top_inc,
    dependencies: libppsdocument_dep,
    c_args: [
      '-DPAPERS_COMPILATION',
      '-DTESTDATADIR="@0@"'.format(join_paths(source_root, 'test-data')),
    ],
  )
  test('pps-@0@-test'.format(name), test_executable, env: backend_env, depends: document_worker)
endforeach

foreach name: ['buffer', 'info', 'geometry', 'objects', 'link-variant']
  test_executable = executable(
    'test-pps-' + name,
    'test-pps-@0@.c'.format(name),
    include_directories: top_inc,
    dependencies: libppsdocument_dep,
    c_args: '-DPAPERS_COMPILATION',
  )
  test('pps-@0@-test'.format(name), test_executable)
endforeach

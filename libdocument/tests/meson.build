foreach name: ['worker', 'proxy', 'export']
  test_cflags = [
    '-DPAPERS_COMPILATION',
    '-DTESTDATADIR="@0@"'.format(join_paths(source_root, 'test-data')),
  ]
  if name == 'export'
    test_cflags += [
      '-DTEST_HAVE_TIFF=@0@'.format(enable_tiff.to_int()),
      '-DTEST_HAVE_DJVU=@0@'.format(enable_djvu.to_int()),
    ]
  endif
  test_executable = executable(
    'test-pps-' + name,
    'test-pps-@0@.c'.format(name),
    document_dbus[1],
    include_directories: top_inc,
    dependencies: libppsdocument_dep,
    c_args: test_cflags,
  )
  test('pps-@0@-test'.format(name), test_executable, env: backend_env, depends: document_worker)
endforeach

foreach name: ['buffer', 'info', 'geometry', 'objects', 'link-variant', 'form-variant', 'attachment-variant', 'annotation-variant', 'transition-variant', 'tree']
  test_executable = executable(
    'test-pps-' + name,
    'test-pps-@0@.c'.format(name),
    include_directories: top_inc,
    dependencies: libppsdocument_dep,
    c_args: '-DPAPERS_COMPILATION',
  )
  test('pps-@0@-test'.format(name), test_executable)
endforeach

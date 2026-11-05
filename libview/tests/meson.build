cflags = [
  '-DG_LOG_DOMAIN="PapersViewTests"',
  '-DPAPERS_COMPILATION',
  '-DTESTDATADIR="@0@"'.format(join_paths(source_root, 'test-data'))
]

test_executable = executable(
  'test-pps-jobs',
  'test-pps-jobs.c',
  include_directories: [top_inc],
  dependencies: libppsview_dep,
  c_args: cflags
)

test('pps-jobs-test', test_executable, env: backend_env)

proxy_annotations_test = executable(
  'test-pps-proxy-annotations',
  'test-pps-proxy-annotations.c',
  include_directories: top_inc,
  dependencies: libppsview_dep,
  c_args: cflags,
)

test('pps-proxy-annotations-test', proxy_annotations_test, env: backend_env, depends: document_worker)

proxy_export_test = executable(
  'test-pps-proxy-export',
  'test-pps-proxy-export.c',
  include_directories: top_inc,
  dependencies: libppsview_dep,
  c_args: cflags,
)

test('pps-proxy-export-test', proxy_export_test, env: backend_env, depends: document_worker)

job_scheduler_test = executable(
  'test-pps-job-scheduler',
  'test-pps-job-scheduler.c',
  include_directories: top_inc,
  dependencies: libppsview_dep,
  c_args: cflags,
)

test('pps-job-scheduler-test', job_scheduler_test, env: backend_env, depends: document_worker)

if enable_gtk_unix_print
  print_operation_test = executable(
    'test-pps-print-operation',
    'test-pps-print-operation.c',
    include_directories: top_inc,
    dependencies: [libppsview_dep, gtk_unix_print_dep],
    c_args: cflags,
  )
  test('pps-print-operation-gui', print_operation_test, suite: 'gui', is_parallel: false, env: backend_env, depends: document_worker)
endif

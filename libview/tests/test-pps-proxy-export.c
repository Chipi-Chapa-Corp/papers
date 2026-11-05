// SPDX-License-Identifier: GPL-2.0-or-later

#include <glib/gstdio.h>
#include <papers-view.h>

#include "pps-document-proxy-private.h"
#include "pps-file-exporter-private.h"

static PpsDocument *
open_document (void)
{
	g_autoptr (GError) error = NULL;
	PpsDocument *document = _pps_document_proxy_new ("application/pdf", NULL, &error);
	g_assert_no_error (error);
	g_autofree char *uri = g_filename_to_uri (TESTDATADIR "/utf16le-annot.pdf", NULL, NULL);
	g_assert_true (pps_document_load (document, uri, &error));
	g_assert_no_error (error);
	pps_document_setup_cache (document);
	return document;
}

static void
run_job (PpsJob *job)
{
	pps_job_export_set_page (PPS_JOB_EXPORT (job), 0);
	pps_job_run (job);
	while (g_main_context_iteration (NULL, FALSE)) {
	}
	g_assert_true (pps_job_is_finished (job));
}

static void
job_failure_and_retry (gconstpointer data)
{
	gboolean invalid_context = GPOINTER_TO_INT (data);
	g_autoptr (GError) error = NULL;
	g_autofree char *directory = g_dir_make_tmp ("papers-export-job-XXXXXX", &error);
	g_assert_no_error (error);
	g_autofree char *valid_path = g_build_filename (directory, "export.pdf", NULL);
	g_autofree char *missing_path = g_build_filename (directory, "missing", "export.pdf", NULL);
	g_autoptr (PpsDocument) document = open_document ();
	PpsFileExporter *exporter = PPS_FILE_EXPORTER (document);
	PpsFileExporterContext context = { PPS_FILE_FORMAT_PDF, invalid_context ? valid_path : missing_path, 0, 0, 200, 200, FALSE, 1 };
	pps_file_exporter_begin (exporter, &context);
	if (invalid_context) {
		g_autoptr (PpsPage) page = pps_document_get_page (document, 0);
		g_autoptr (PpsRenderContext) rc = pps_render_context_new (page, 45, 1, PPS_RENDER_ANNOTS_ALL);
		pps_file_exporter_do_page (exporter, rc);
	}
	g_autoptr (PpsJob) job = pps_job_export_new (document);
	run_job (job);
	g_assert_false (pps_job_is_succeeded (job, &error));
	GIOErrorEnum expected = invalid_context ? G_IO_ERROR_INVALID_ARGUMENT : G_IO_ERROR_NOT_FOUND;
	g_assert_error (error, G_IO_ERROR, expected);
	g_clear_error (&error);
	pps_file_exporter_end (exporter);
	context.filename = valid_path;
	pps_file_exporter_begin (exporter, &context);
	g_assert_false (_pps_file_exporter_get_error (exporter, NULL));
	pps_file_exporter_begin_page (exporter);
	run_job (job);
	g_assert_true (pps_job_is_succeeded (job, &error));
	g_assert_no_error (error);
	pps_file_exporter_end_page (exporter);
	pps_file_exporter_end (exporter);
	g_assert_false (_pps_file_exporter_get_error (exporter, NULL));
	g_autoptr (PpsDocument) saved = _pps_document_proxy_new ("application/pdf", NULL, &error);
	g_assert_no_error (error);
	g_autofree char *uri = g_filename_to_uri (valid_path, NULL, NULL);
	g_assert_true (pps_document_load (saved, uri, &error));
	g_assert_no_error (error);
	pps_document_setup_cache (saved);
	g_assert_cmpint (pps_document_get_n_pages (saved), ==, 1);
	g_assert_cmpuint (g_type_from_name ("PdfDocument"), ==, G_TYPE_INVALID);
	g_assert_cmpint (g_unlink (valid_path), ==, 0);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}

static void
worker_failure (void)
{
	g_autoptr (GError) error = NULL;
	g_autofree char *directory = g_dir_make_tmp ("papers-export-job-XXXXXX", &error);
	g_assert_no_error (error);
	g_autofree char *path = g_build_filename (directory, "export.pdf", NULL);
	g_autoptr (PpsDocument) document = open_document ();
	PpsFileExporter *exporter = PPS_FILE_EXPORTER (document);
	PpsFileExporterContext context = { PPS_FILE_FORMAT_PDF, path, 0, 0, 200, 200, FALSE, 1 };
	pps_file_exporter_begin (exporter, &context);
	g_assert_false (_pps_file_exporter_get_error (exporter, NULL));
	GSubprocess *process = _pps_document_proxy_get_session (document)->process;
	g_subprocess_force_exit (process);
	g_assert_true (g_subprocess_wait (process, NULL, &error));
	g_assert_no_error (error);
	g_autoptr (PpsJob) job = pps_job_export_new (document);
	run_job (job);
	g_assert_false (pps_job_is_succeeded (job, &error));
	g_assert_nonnull (error);
	g_assert_cmpuint (error->domain, ==, G_IO_ERROR);
	g_clear_error (&error);
	pps_file_exporter_end (exporter);
	g_autoptr (PpsDocument) second = open_document ();
	PpsFileExporter *other = PPS_FILE_EXPORTER (second);
	pps_file_exporter_begin (other, &context);
	pps_file_exporter_begin_page (other);
	g_autoptr (PpsJob) other_job = pps_job_export_new (second);
	run_job (other_job);
	g_assert_true (pps_job_is_succeeded (other_job, &error));
	g_assert_no_error (error);
	pps_file_exporter_end_page (other);
	pps_file_exporter_end (other);
	g_assert_false (_pps_file_exporter_get_error (other, NULL));
	g_assert_cmpint (g_unlink (path), ==, 0);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_assert_true (pps_init ());
	g_test_add_data_func ("/export-job/output-failure-and-retry", GINT_TO_POINTER (FALSE), job_failure_and_retry);
	g_test_add_data_func ("/export-job/remote-failure-and-retry", GINT_TO_POINTER (TRUE), job_failure_and_retry);
	g_test_add_func ("/export-job/worker-failure", worker_failure);
	int result = g_test_run ();
	pps_shutdown ();
	return result;
}

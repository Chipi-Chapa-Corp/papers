// SPDX-License-Identifier: GPL-2.0-or-later

#include <cairo-pdf.h>
#include <glib/gstdio.h>
#include <gtk/gtkunixprint.h>
#include <papers-view.h>

#include "pps-file-exporter-private.h"

typedef struct {
	PpsDocument parent;
	cairo_surface_t *surface;
	cairo_t *cr;
	const char *failure;
	guint begins, ends;
} TestDocument;

typedef PpsDocumentClass TestDocumentClass;
static GType test_document_get_type (void);
static void exporter_iface_init (PpsFileExporterInterface *iface);
G_DEFINE_TYPE_WITH_CODE (TestDocument, test_document, PPS_TYPE_DOCUMENT, G_IMPLEMENT_INTERFACE (PPS_TYPE_FILE_EXPORTER, exporter_iface_init))

static gboolean
load_document (PpsDocument *document, const char *uri, GError **error)
{
	return TRUE;
}

static int
get_n_pages (PpsDocument *document)
{
	return 1;
}

static void
get_page_size (PpsDocument *document, PpsPage *page, double *width, double *height)
{
	*width = *height = 200;
}

static void
test_document_class_init (TestDocumentClass *klass)
{
	klass->load = load_document;
	klass->get_n_pages = get_n_pages;
	klass->get_page_size = get_page_size;
}

static void
test_document_init (TestDocument *document)
{
}

static void
fail_phase (PpsFileExporter *exporter, const char *phase)
{
	TestDocument *document = (TestDocument *) exporter;
	if (document->begins == 1 && g_strcmp0 (document->failure, phase) == 0)
		_pps_file_exporter_take_error (exporter, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, phase));
}

static void
begin_export (PpsFileExporter *exporter, PpsFileExporterContext *context)
{
	TestDocument *document = (TestDocument *) exporter;
	g_assert_null (document->surface);
	document->begins++;
	document->surface = cairo_pdf_surface_create (context->filename, context->paper_width, context->paper_height);
	document->cr = cairo_create (document->surface);
	fail_phase (exporter, "begin");
}

static void
begin_page (PpsFileExporter *exporter)
{
	fail_phase (exporter, "begin-page");
}

static void
export_page (PpsFileExporter *exporter, PpsRenderContext *context)
{
	TestDocument *document = (TestDocument *) exporter;
	cairo_rectangle (document->cr, 10, 10, 50, 50);
	cairo_fill (document->cr);
	fail_phase (exporter, "page");
}

static void
end_page (PpsFileExporter *exporter)
{
	TestDocument *document = (TestDocument *) exporter;
	cairo_show_page (document->cr);
	fail_phase (exporter, "end-page");
}

static void
end_export (PpsFileExporter *exporter)
{
	TestDocument *document = (TestDocument *) exporter;
	g_assert_nonnull (document->surface);
	document->ends++;
	cairo_destroy (document->cr);
	cairo_surface_destroy (document->surface);
	document->cr = NULL;
	document->surface = NULL;
	fail_phase (exporter, "end");
}

static PpsFileExporterCapabilities
get_capabilities (PpsFileExporter *exporter)
{
	return PPS_FILE_EXPORTER_CAN_GENERATE_PDF;
}

static void
exporter_iface_init (PpsFileExporterInterface *iface)
{
	iface->begin = begin_export;
	iface->begin_page = begin_page;
	iface->do_page = export_page;
	iface->end_page = end_page;
	iface->end = end_export;
	iface->get_capabilities = get_capabilities;
}

typedef struct {
	GMainLoop *loop;
	GtkPrintOperationResult result;
	GError *error;
	guint done_count;
} PrintResult;

static void
print_done (PpsPrintOperation *op, GtkPrintOperationResult result, PrintResult *state)
{
	state->done_count++;
	state->result = result;
	if (result == GTK_PRINT_OPERATION_RESULT_ERROR)
		pps_print_operation_get_error (op, &state->error);
	g_main_loop_quit (state->loop);
}

static gboolean
accept_file_printer (gpointer data)
{
	GListModel *windows = gtk_window_get_toplevels ();
	for (guint i = 0; i < g_list_model_get_n_items (windows); i++) {
		g_autoptr (GtkWindow) window = g_list_model_get_item (windows, i);
		if (!GTK_IS_PRINT_UNIX_DIALOG (window))
			continue;
		GtkPrinter *printer = gtk_print_unix_dialog_get_selected_printer (GTK_PRINT_UNIX_DIALOG (window));
		if (!printer)
			return G_SOURCE_CONTINUE;
		/* The test must never submit to a real printer. */
		g_assert_cmpstr (gtk_printer_get_name (printer), ==, "Print to File");
		g_signal_emit_by_name (window, "response", GTK_RESPONSE_OK);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static gboolean
print_timed_out (gpointer data)
{
	g_error ("Print test did not finish");
	return G_SOURCE_REMOVE;
}

static void
export_failure (gconstpointer data)
{
	TestDocument *document = g_object_new (test_document_get_type (), NULL);
	g_autoptr (GError) error = NULL;
	g_autofree char *source_uri = g_filename_to_uri (TESTDATADIR "/utf16le-annot.pdf", NULL, NULL);
	g_assert_true (pps_document_load (PPS_DOCUMENT (document), source_uri, &error));
	g_assert_no_error (error);
	pps_document_setup_cache (PPS_DOCUMENT (document));
	document->failure = data;
	g_autofree char *directory = g_dir_make_tmp ("papers-print-test-XXXXXX", &error);
	g_assert_no_error (error);
	g_autofree char *path = g_build_filename (directory, "printed.pdf", NULL);
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	g_autoptr (GtkPrintSettings) settings = gtk_print_settings_new ();
	gtk_print_settings_set_printer (settings, "Print to File");
	gtk_print_settings_set (settings, GTK_PRINT_SETTINGS_OUTPUT_URI, uri);
	gtk_print_settings_set (settings, GTK_PRINT_SETTINGS_OUTPUT_FILE_FORMAT, "pdf");
	PpsPrintOperation *op = pps_print_operation_new (PPS_DOCUMENT (document));
	pps_print_operation_set_print_settings (op, settings);
	pps_print_operation_set_job_name (op, "Papers test");
	g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
	PrintResult state = { .loop = loop };
	g_signal_connect (op, "done", G_CALLBACK (print_done), &state);
	GtkWindow *parent = GTK_WINDOW (gtk_window_new ());
	gtk_window_present (parent);
	guint timeout = g_timeout_add_seconds (10, print_timed_out, NULL);
	g_timeout_add (10, accept_file_printer, NULL);
	pps_print_operation_run (op, parent);
	g_main_loop_run (loop);
	g_source_remove (timeout);
	g_assert_cmpuint (state.done_count, ==, 1);
	if (data) {
		g_assert_cmpint (state.result, ==, GTK_PRINT_OPERATION_RESULT_ERROR);
		g_assert_error (state.error, G_IO_ERROR, G_IO_ERROR_FAILED);
		g_assert_cmpstr (state.error->message, ==, data);
		g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
	} else {
		g_assert_cmpint (state.result, ==, GTK_PRINT_OPERATION_RESULT_APPLY);
		g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));
		g_assert_cmpint (g_unlink (path), ==, 0);
	}
	g_assert_cmpuint (document->begins, ==, 1);
	g_assert_cmpuint (document->ends, ==, 1);
	g_clear_error (&state.error);
	g_object_unref (op);
	g_object_unref (document);
	gtk_window_destroy (parent);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	gtk_init ();
	g_test_add_data_func ("/print-export/success", NULL, export_failure);
	g_test_add_data_func ("/print-export/begin-error", "begin", export_failure);
	g_test_add_data_func ("/print-export/begin-page-error", "begin-page", export_failure);
	g_test_add_data_func ("/print-export/page-error", "page", export_failure);
	g_test_add_data_func ("/print-export/end-page-error", "end-page", export_failure);
	g_test_add_data_func ("/print-export/end-error", "end", export_failure);
	return g_test_run ();
}

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
	GMutex mutex;
	GCond condition;
	gboolean pause, entered, release;
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
test_document_finalize (GObject *object)
{
	TestDocument *document = (TestDocument *) object;
	g_assert_null (document->surface);
	g_mutex_clear (&document->mutex);
	g_cond_clear (&document->condition);
	G_OBJECT_CLASS (test_document_parent_class)->finalize (object);
}

static void
test_document_class_init (TestDocumentClass *klass)
{
	G_OBJECT_CLASS (klass)->finalize = test_document_finalize;
	klass->load = load_document;
	klass->get_n_pages = get_n_pages;
	klass->get_page_size = get_page_size;
}

static void
test_document_init (TestDocument *document)
{
	g_mutex_init (&document->mutex);
	g_cond_init (&document->condition);
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
	g_mutex_lock (&document->mutex);
	if (document->pause && document->begins == 1) {
		document->entered = TRUE;
		while (!document->release)
			g_cond_wait (&document->condition, &document->mutex);
	}
	g_mutex_unlock (&document->mutex);
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
	guint *remaining;
	gboolean ready;
} PrintResult;

static void
print_done (PpsPrintOperation *op, GtkPrintOperationResult result, PrintResult *state)
{
	state->done_count++;
	state->result = result;
	if (result == GTK_PRINT_OPERATION_RESULT_ERROR)
		pps_print_operation_get_error (op, &state->error);
	if (!state->remaining || --*state->remaining == 0)
		g_main_loop_quit (state->loop);
}

static void
print_ready (PpsPrintOperation *op, PrintResult *state)
{
	state->ready = TRUE;
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
		return data ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static gboolean
print_timed_out (gpointer data)
{
	g_error ("Print test did not finish");
	return G_SOURCE_REMOVE;
}

static TestDocument *
new_document (void)
{
	TestDocument *document = g_object_new (test_document_get_type (), NULL);
	g_autoptr (GError) error = NULL;
	g_autofree char *uri = g_filename_to_uri (TESTDATADIR "/utf16le-annot.pdf", NULL, NULL);
	g_assert_true (pps_document_load (PPS_DOCUMENT (document), uri, &error));
	g_assert_no_error (error);
	pps_document_setup_cache (PPS_DOCUMENT (document));
	return document;
}

static PpsPrintOperation *
start_print (TestDocument *document, GtkWindow *parent, const char *path, PrintResult *state)
{
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	g_autoptr (GtkPrintSettings) settings = gtk_print_settings_new ();
	gtk_print_settings_set_printer (settings, "Print to File");
	gtk_print_settings_set (settings, GTK_PRINT_SETTINGS_OUTPUT_URI, uri);
	gtk_print_settings_set (settings, GTK_PRINT_SETTINGS_OUTPUT_FILE_FORMAT, "pdf");
	PpsPrintOperation *op = pps_print_operation_new (PPS_DOCUMENT (document));
	pps_print_operation_set_print_settings (op, settings);
	pps_print_operation_set_job_name (op, "Papers test");
	g_signal_connect (op, "done", G_CALLBACK (print_done), state);
	g_signal_connect (op, "begin-print", G_CALLBACK (print_ready), state);
	pps_print_operation_run (op, parent);
	return op;
}

static void
export_failure (gconstpointer data)
{
	TestDocument *document = new_document ();
	g_autoptr (GError) error = NULL;
	document->failure = data;
	g_autofree char *directory = g_dir_make_tmp ("papers-print-test-XXXXXX", &error);
	g_assert_no_error (error);
	g_autofree char *path = g_build_filename (directory, "printed.pdf", NULL);
	g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
	PrintResult state = { .loop = loop };
	GtkWindow *parent = GTK_WINDOW (gtk_window_new ());
	gtk_window_present (parent);
	guint timeout = g_timeout_add_seconds (10, print_timed_out, NULL);
	g_timeout_add (10, accept_file_printer, NULL);
	PpsPrintOperation *op = start_print (document, parent, path, &state);
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

typedef struct {
	TestDocument *document;
	GtkWindow *parent;
	PpsPrintOperation *ops[3];
	PrintResult states[3];
	char *paths[3];
	guint phase;
	gboolean queued;
} CancelQueue;

static gboolean
cancel_queued_export (gpointer data)
{
	CancelQueue *state = data;
	TestDocument *document = state->document;
	g_mutex_lock (&document->mutex);
	gboolean entered = document->entered;
	g_mutex_unlock (&document->mutex);
	if (!entered)
		return G_SOURCE_CONTINUE;
	if (state->phase == 0) {
		state->ops[1] = start_print (document, state->parent, state->paths[1], &state->states[1]);
		state->phase++;
	} else if (state->phase == 1 && state->states[1].ready) {
		if (state->queued) {
			state->ops[2] = start_print (document, state->parent, state->paths[2], &state->states[2]);
		} else {
			pps_print_operation_cancel (state->ops[0]);
			g_assert_cmpuint (state->states[0].done_count, ==, 0);
		}
		state->phase++;
	} else if (state->phase == 2 && (!state->queued || state->states[2].ready)) {
		if (state->queued) {
			pps_print_operation_cancel (state->ops[2]);
			g_assert_cmpuint (state->states[2].done_count, ==, 1);
			g_assert_cmpint (state->states[2].result, ==, GTK_PRINT_OPERATION_RESULT_CANCEL);
		}
		g_assert_cmpuint (document->begins, ==, 1);
		g_assert_cmpuint (document->ends, ==, 0);
		g_mutex_lock (&document->mutex);
		document->release = TRUE;
		g_cond_signal (&document->condition);
		g_mutex_unlock (&document->mutex);
		state->phase++;
	}
	return G_SOURCE_CONTINUE;
}

static void
cancel_export (gconstpointer data)
{
	TestDocument *document = new_document ();
	document->pause = TRUE;
	g_autoptr (GError) error = NULL;
	g_autofree char *directory = g_dir_make_tmp ("papers-print-queue-XXXXXX", &error);
	g_assert_no_error (error);
	g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
	guint remaining = GPOINTER_TO_INT (data) ? 3 : 2;
	guint n_ops = remaining;
	CancelQueue state = { .document = document, .queued = GPOINTER_TO_INT (data) };
	state.parent = GTK_WINDOW (gtk_window_new ());
	gtk_window_present (state.parent);
	for (guint i = 0; i < n_ops; i++) {
		state.paths[i] = g_strdup_printf ("%s/printed-%u.pdf", directory, i);
		state.states[i].loop = loop;
		state.states[i].remaining = &remaining;
	}
	guint timeout = g_timeout_add_seconds (10, print_timed_out, NULL);
	guint accept = g_timeout_add (10, accept_file_printer, &state);
	guint control = g_timeout_add (10, cancel_queued_export, &state);
	state.ops[0] = start_print (document, state.parent, state.paths[0], &state.states[0]);
	g_main_loop_run (loop);
	g_source_remove (control);
	g_source_remove (accept);
	g_source_remove (timeout);
	for (guint i = 0; i < n_ops; i++) {
		g_assert_cmpuint (state.states[i].done_count, ==, 1);
		gboolean cancelled = state.queued ? i == 2 : i == 0;
		GtkPrintOperationResult expected = cancelled ? GTK_PRINT_OPERATION_RESULT_CANCEL : GTK_PRINT_OPERATION_RESULT_APPLY;
		g_assert_cmpint (state.states[i].result, ==, expected);
		g_assert_no_error (state.states[i].error);
		g_assert_cmpint (g_file_test (state.paths[i], G_FILE_TEST_EXISTS), ==, !cancelled);
		if (!cancelled)
			g_assert_cmpint (g_unlink (state.paths[i]), ==, 0);
		g_free (state.paths[i]);
		g_object_unref (state.ops[i]);
	}
	g_assert_cmpuint (document->begins, ==, 2);
	g_assert_cmpuint (document->ends, ==, 2);
	g_object_unref (document);
	gtk_window_destroy (state.parent);
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
	g_test_add_data_func ("/print-export/cancel-active", GINT_TO_POINTER (FALSE), cancel_export);
	g_test_add_data_func ("/print-export/cancel-queued", GINT_TO_POINTER (TRUE), cancel_export);
	return g_test_run ();
}

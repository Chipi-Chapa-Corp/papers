// SPDX-License-Identifier: GPL-2.0-or-later

#include <cairo-pdf.h>
#include <fcntl.h>
#include <gio/gunixfdlist.h>
#include <glib/gstdio.h>
#include <gtk/gtkunixprint.h>
#include <papers-view.h>
#include <unistd.h>

#include "pps-document-factory-private.h"
#include "pps-file-exporter-private.h"
#include "pps-print-operation-private.h"
#ifdef GDK_WINDOWING_X11
#include <X11/Xutil.h>
#include <gdk/x11/gdkx.h>
#endif

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

typedef struct {
	GtkWindow *parent;
	PpsPrintOperation *op;
	gboolean cancel;
	gboolean preview;
} NativeDialog;

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static gboolean
respond_native_dialog (gpointer data)
{
	NativeDialog *state = data;
	GListModel *windows = gtk_window_get_toplevels ();
	for (guint i = 0; i < g_list_model_get_n_items (windows); i++) {
		g_autoptr (GtkWindow) window = g_list_model_get_item (windows, i);
		if (!GTK_IS_PRINT_UNIX_DIALOG (window) || !gtk_widget_get_mapped (GTK_WIDGET (window)))
			continue;
		GtkPrinter *printer = gtk_print_unix_dialog_get_selected_printer (GTK_PRINT_UNIX_DIALOG (window));
		if (!printer)
			return G_SOURCE_CONTINUE;
		g_assert_cmpstr (gtk_printer_get_name (printer), ==, "Print to File");
#ifdef GDK_WINDOWING_X11
		GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (window));
		if (GDK_IS_X11_SURFACE (surface)) {
			Window parent;
			g_assert_true (XGetTransientForHint (GDK_SURFACE_XDISPLAY (surface), GDK_SURFACE_XID (surface), &parent));
			g_assert_cmpuint (parent, ==, GDK_SURFACE_XID (gtk_native_get_surface (GTK_NATIVE (state->parent))));
		}
#endif
		if (state->cancel)
			pps_print_operation_cancel (state->op);
		else
			g_signal_emit_by_name (window, "response", state->preview ? GTK_RESPONSE_APPLY : GTK_RESPONSE_OK);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}
G_GNUC_END_IGNORE_DEPRECATIONS

static void
native_dialog (gconstpointer data)
{
	g_autoptr (GError) error = NULL;
	g_autoptr (PpsDocument) document = _pps_document_factory_create_document ("application/pdf", &error);
	g_assert_no_error (error);
	g_autofree char *uri = g_filename_to_uri (TESTDATADIR "/ink_annotations.pdf", NULL, NULL);
	g_assert_true (pps_document_load (document, uri, &error));
	g_assert_no_error (error);
	pps_document_setup_cache (document);
	g_autofree char *directory = g_dir_make_tmp ("papers-native-print-XXXXXX", &error);
	g_assert_no_error (error);
	g_autofree char *path = g_build_filename (directory, "printed.pdf", NULL);
	g_autofree char *output = g_filename_to_uri (path, NULL, NULL);
	g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
	PrintResult result = { .loop = loop };
	GtkWindow *parent = GTK_WINDOW (gtk_window_new ());
	gtk_window_present (parent);
	char *handle = NULL;
#ifdef GDK_WINDOWING_X11
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (parent));
	if (GDK_IS_X11_SURFACE (surface))
		handle = g_strdup_printf ("x11:%lx", GDK_SURFACE_XID (surface));
	G_GNUC_END_IGNORE_DEPRECATIONS
#endif
	g_autoptr (GtkPrintSettings) settings = gtk_print_settings_new ();
	gtk_print_settings_set_printer (settings, "Print to File");
	gtk_print_settings_set (settings, GTK_PRINT_SETTINGS_OUTPUT_URI, output);
	gtk_print_settings_set (settings, GTK_PRINT_SETTINGS_OUTPUT_FILE_FORMAT, "pdf");
	gtk_print_settings_set_bool (settings, "papers-print-setting-page-size", TRUE);
	gtk_print_settings_set_bool (settings, "papers-print-setting-page-draw-borders", TRUE);
	PpsPrintOperation *op = _pps_print_operation_new_for_worker (document, handle);
	g_free (handle);
	pps_print_operation_set_current_page (op, 0);
	pps_print_operation_set_job_name (op, "Native print test");
	pps_print_operation_set_print_settings (op, settings);
	g_signal_connect (op, "done", G_CALLBACK (print_done), &result);
	NativeDialog state = { .parent = parent, .op = op, .cancel = GPOINTER_TO_INT (data) == 1, .preview = GPOINTER_TO_INT (data) == 2 };
	g_autofree char *preview_command = NULL;
	if (state.preview) {
		g_autofree char *executable = g_file_read_link ("/proc/self/exe", &error);
		g_assert_no_error (error);
		g_autofree char *quoted = g_shell_quote (executable);
		g_autofree char *destination = g_shell_quote (path);
		g_autofree char *command = g_strdup_printf ("%s --preview-fixture %%f %%s %s", quoted, destination);
		g_object_get (gtk_settings_get_default (), "gtk-print-preview-command", &preview_command, NULL);
		g_object_set (gtk_settings_get_default (), "gtk-print-preview-command", command, NULL);
	}
	guint timeout = g_timeout_add_seconds (10, print_timed_out, NULL);
	g_timeout_add (10, respond_native_dialog, &state);
	pps_print_operation_run (op, NULL);
	if (!result.done_count)
		g_main_loop_run (loop);
	g_source_remove (timeout);
	g_assert_cmpuint (result.done_count, ==, 1);
	g_assert_no_error (result.error);
	g_assert_cmpint (result.result, ==, state.cancel ? GTK_PRINT_OPERATION_RESULT_CANCEL : GTK_PRINT_OPERATION_RESULT_APPLY);
	if (!state.cancel) {
		/* GTK reports submission before the file printer's asynchronous write. */
		gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
		gboolean complete = FALSE;
		while (!complete && g_get_monotonic_time () < deadline) {
			g_autofree char *contents = NULL;
			gsize length;
			complete = g_file_get_contents (path, &contents, &length, NULL) &&
			           length >= 6 && memcmp (contents + length - 6, "%%EOF\n", 6) == 0;
			while (g_main_context_iteration (NULL, FALSE)) {
			}
			g_usleep (1000);
		}
		g_assert_true (complete);
		g_autoptr (PpsDocument) printed = _pps_document_factory_create_document ("application/pdf", &error);
		g_assert_true (pps_document_load (printed, output, &error));
		pps_document_setup_cache (printed);
		g_assert_no_error (error);
		g_assert_nonnull (printed);
		g_autoptr (PpsPage) page = pps_document_get_page (printed, 0);
		g_autofree char *text = pps_document_text_get_text (PPS_DOCUMENT_TEXT (printed), page);
		g_assert_nonnull (strstr (text, "These strokes are"));
		g_assert_cmpint (g_unlink (path), ==, 0);
	} else {
		g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
	}
	if (state.preview)
		g_object_set (gtk_settings_get_default (), "gtk-print-preview-command", preview_command, NULL);
	g_object_unref (op);
	gtk_window_destroy (parent);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}

static gboolean
cancel_remote_dialog (gpointer data)
{
	NativeDialog *state = data;
	if (state->cancel)
		gtk_window_destroy (state->parent);
	else
		pps_print_operation_cancel (state->op);
	return G_SOURCE_REMOVE;
}

static void
remote_dialog_cancel (gconstpointer data)
{
	g_autoptr (GError) error = NULL;
	g_autofree char *uri = g_filename_to_uri (TESTDATADIR "/ink_annotations.pdf", NULL, NULL);
	g_autoptr (PpsDocument) document = pps_document_factory_get_document (uri, &error);
	g_assert_no_error (error);
	g_assert_true (pps_document_load (document, uri, &error));
	g_assert_no_error (error);
	pps_document_setup_cache (document);
	g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
	PrintResult result = { .loop = loop };
	GtkWindow *parent = GTK_WINDOW (gtk_window_new ());
	gtk_window_present (parent);
	PpsPrintOperation *op = pps_print_operation_new (document);
	pps_print_operation_set_job_name (op, "Remote cancellation test");
	g_signal_connect (op, "done", G_CALLBACK (print_done), &result);
	NativeDialog state = { .parent = parent, .op = op, .cancel = GPOINTER_TO_INT (data) };
	guint timeout = g_timeout_add_seconds (10, print_timed_out, NULL);
	g_timeout_add (400, cancel_remote_dialog, &state);
	pps_print_operation_run (op, parent);
	g_main_loop_run (loop);
	g_source_remove (timeout);
	g_assert_cmpuint (result.done_count, ==, 1);
	g_assert_cmpint (result.result, ==, GTK_PRINT_OPERATION_RESULT_CANCEL);
	g_assert_no_error (result.error);
	g_object_unref (op);
	if (!state.cancel)
		gtk_window_destroy (parent);
}

static GDBusMessage *
check_portal_parent (GDBusConnection *connection, GDBusMessage *message, gboolean incoming, gpointer data)
{
	if (incoming || g_strcmp0 (g_dbus_message_get_interface (message), "org.freedesktop.portal.Print"))
		return message;
	gboolean print = g_str_equal (g_dbus_message_get_member (message), "Print");
	g_autoptr (GVariant) expected = g_variant_ref_sink (print ? g_variant_new ("(ssha{sv})", "x11:1234", "Test title", 0, NULL) : g_variant_new ("(ssa{sv}a{sv}a{sv})", "x11:1234", "Test title", NULL, NULL, NULL));
	g_assert_true (g_variant_equal (g_dbus_message_get_body (message), expected));
	g_assert_cmpuint (g_dbus_message_get_serial (message), >, 0);
	GUnixFDList *fds = g_dbus_message_get_unix_fd_list (message);
	if (print) {
		g_assert_nonnull (fds);
		g_assert_cmpint (g_unix_fd_list_get_length (fds), ==, 1);
		int fd = g_unix_fd_list_get (fds, 0, NULL);
		g_assert_cmpint (fd, >=, 0);
		g_assert_cmpint (fcntl (fd, F_GETFD), !=, -1);
		close (fd);
	} else {
		g_assert_null (fds);
	}
	g_atomic_int_inc ((int *) data);
	g_object_unref (message);
	return NULL;
}

static void
portal_parent (void)
{
	g_autoptr (GError) error = NULL;
	g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
	g_assert_no_error (error);
	TestDocument *document = new_document ();
	PpsPrintOperation *op = _pps_print_operation_new_for_worker (PPS_DOCUMENT (document), "x11:1234");
	int checked = 0;
	guint filter = g_dbus_connection_add_filter (bus, check_portal_parent, &checked, NULL);
	g_autoptr (GUnixFDList) fds = g_unix_fd_list_new ();
	int fd = open ("/dev/null", O_RDONLY | O_CLOEXEC);
	g_assert_cmpint (fd, >=, 0);
	g_assert_cmpint (g_unix_fd_list_append (fds, fd, &error), ==, 0);
	close (fd);
	g_assert_no_error (error);
	for (guint i = 0; i < 2; i++) {
		g_autoptr (GDBusMessage) message = g_dbus_message_new_method_call ("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Print", i ? "Print" : "PreparePrint");
		g_dbus_message_set_body (message, i ? g_variant_new ("(ssha{sv})", "", "Test title", 0, NULL) : g_variant_new ("(ssa{sv}a{sv}a{sv})", "", "Test title", NULL, NULL, NULL));
		if (i)
			g_dbus_message_set_unix_fd_list (message, fds);
		g_assert_true (g_dbus_connection_send_message (bus, message, G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, &error));
		g_assert_no_error (error);
	}
	gint64 deadline = g_get_monotonic_time () + 3 * G_TIME_SPAN_SECOND;
	while (g_atomic_int_get (&checked) < 2 && g_get_monotonic_time () < deadline) {
		while (g_main_context_iteration (NULL, FALSE)) {
		}
		g_usleep (1000);
	}
	g_assert_cmpint (g_atomic_int_get (&checked), ==, 2);
	g_dbus_connection_remove_filter (bus, filter);
	g_object_unref (op);
	g_object_unref (document);
}

int
main (int argc, char **argv)
{
	if (argc == 5 && g_str_equal (argv[1], "--preview-fixture")) {
		g_autofree char *contents = NULL;
		gsize length;
		if (!g_file_get_contents (argv[2], &contents, &length, NULL) ||
		    !g_file_set_contents (argv[4], contents, length, NULL))
			return 1;
		g_unlink (argv[2]);
		g_unlink (argv[3]);
		return 0;
	}
	g_test_init (&argc, &argv, NULL);
	gtk_init ();
	g_assert_true (pps_init ());
	g_test_add_data_func ("/print-export/success", NULL, export_failure);
	g_test_add_data_func ("/print-export/begin-error", "begin", export_failure);
	g_test_add_data_func ("/print-export/begin-page-error", "begin-page", export_failure);
	g_test_add_data_func ("/print-export/page-error", "page", export_failure);
	g_test_add_data_func ("/print-export/end-page-error", "end-page", export_failure);
	g_test_add_data_func ("/print-export/end-error", "end", export_failure);
	g_test_add_data_func ("/print-export/cancel-active", GINT_TO_POINTER (FALSE), cancel_export);
	g_test_add_data_func ("/print-export/cancel-queued", GINT_TO_POINTER (TRUE), cancel_export);
	g_test_add_data_func ("/print-native/foreign-parent-file", NULL, native_dialog);
	g_test_add_data_func ("/print-native/preview", GINT_TO_POINTER (2), native_dialog);
	g_test_add_func ("/print-native/portal-parent", portal_parent);
	g_test_add_data_func ("/print-native/cancel-dialog", GINT_TO_POINTER (TRUE), native_dialog);
	g_test_add_data_func ("/print-remote/cancel-dialog", NULL, remote_dialog_cancel);
	g_test_add_data_func ("/print-remote/close-parent", GINT_TO_POINTER (TRUE), remote_dialog_cancel);
	int result = g_test_run ();
	pps_job_scheduler_wait ();
	pps_shutdown ();
	return result;
}

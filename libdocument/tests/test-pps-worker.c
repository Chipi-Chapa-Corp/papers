// SPDX-License-Identifier: GPL-2.0-or-later

#include <fcntl.h>
#include <gio/gunixfdlist.h>
#include <math.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pps-document-annotation-private.h"
#include "pps-document-info-private.h"
#include "pps-document-session-private.h"
#include "pps-document.h"

static PpsDocumentSession *
new_session (void)
{
	g_autoptr (GError) error = NULL;
	g_auto (GStrv) interfaces = NULL;
	guint annotation_methods;
	PpsDocumentSession *session = _pps_document_session_new (NULL, &error);

	g_assert_no_error (error);
	g_assert_nonnull (session);
	g_assert_true (pps_dbus_document_call_create_sync (
	    session->proxy, "application/pdf", G_DBUS_CALL_FLAGS_NONE, 5000,
	    &interfaces, &annotation_methods, NULL, &error));
	g_assert_no_error (error);
	g_assert_true (g_strv_contains ((const char *const *) interfaces, "PpsDocumentText"));
	g_assert_true (g_strv_contains ((const char *const *) interfaces, "PpsDocumentSecurity"));
	g_assert_cmpuint (annotation_methods, ==, PPS_ANNOTATION_METHOD_ADD | PPS_ANNOTATION_METHOD_REMOVE | PPS_ANNOTATION_METHOD_OVER_MARKUP);
	return session;
}

static int
load (PpsDocumentSession *session, const char *fixture, const char *password, GError **error)
{
	g_autofree char *path = g_build_filename (TESTDATADIR, fixture, NULL);
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	int n_pages = -1;

	pps_dbus_document_call_load_sync (session->proxy, uri, password,
	                                  G_DBUS_CALL_FLAGS_NONE, 5000,
	                                  &n_pages, NULL, error);
	return n_pages;
}

static void
page_info (PpsDocumentSession *session)
{
	g_autoptr (GError) error = NULL;
	g_autofree char *label = NULL;
	g_autoptr (GVariant) metadata = NULL;
	g_autoptr (PpsDocumentInfo) info = NULL;
	double width, height;

	g_assert_true (pps_dbus_document_call_get_page_info_sync (
	    session->proxy, 0, G_DBUS_CALL_FLAGS_NONE, 5000,
	    &width, &height, &label, NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpfloat (width, >, 0);
	g_assert_cmpfloat (height, >, 0);
	g_assert_nonnull (label);
	g_assert_true (pps_dbus_document_call_get_info_sync (
	    session->proxy, G_DBUS_CALL_FLAGS_NONE, 5000, &metadata,
	    NULL, NULL, NULL, NULL, NULL, &error));
	g_assert_no_error (error);
	info = _pps_document_info_from_variant (metadata, &error);
	g_assert_no_error (error);
	g_assert_nonnull (info);
	g_assert_cmpint (info->n_pages, ==, 1);
}

static void
load_and_password (void)
{
	g_autoptr (PpsDocumentSession) first = new_session ();
	g_autoptr (PpsDocumentSession) second = new_session ();
	g_autoptr (GError) error = NULL;

	g_assert_cmpstr (g_subprocess_get_identifier (first->process), !=,
	                 g_subprocess_get_identifier (second->process));
	g_assert_cmpint (load (first, "utf16le-annot.pdf", "", &error), ==, 1);
	g_assert_no_error (error);
	page_info (first);

	g_assert_cmpint (load (second, "PasswordEncrypted.pdf", "", &error), ==, -1);
	g_assert_error (error, PPS_DOCUMENT_ERROR, PPS_DOCUMENT_ERROR_ENCRYPTED);
	g_clear_error (&error);
	g_assert_cmpint (load (second, "PasswordEncrypted.pdf", "wrong", &error), ==, -1);
	g_assert_error (error, PPS_DOCUMENT_ERROR, PPS_DOCUMENT_ERROR_ENCRYPTED);
	g_clear_error (&error);
	page_info (first);
	g_assert_cmpint (load (second, "PasswordEncrypted.pdf", "password", &error), ==, 1);
	g_assert_no_error (error);
	page_info (second);
	/* No parser type was registered in the calling process. */
	g_assert_cmpuint (g_type_from_name ("PdfDocument"), ==, G_TYPE_INVALID);
}

static void
load_fd (void)
{
	g_autoptr (PpsDocumentSession) session = new_session ();
	g_autoptr (GUnixFDList) fd_list = g_unix_fd_list_new ();
	g_autoptr (GError) error = NULL;
	int n_pages, handle;
	int fd = open (TESTDATADIR "/utf16le-annot.pdf", O_RDONLY | O_CLOEXEC);

	g_assert_cmpint (fd, >=, 0);
	handle = g_unix_fd_list_append (fd_list, fd, &error);
	close (fd);
	g_assert_no_error (error);
	g_assert_false (pps_dbus_document_call_load_fd_sync (
	    session->proxy, g_variant_new_handle (handle + 1), "",
	    G_DBUS_CALL_FLAGS_NONE, 5000, fd_list,
	    &n_pages, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	g_assert_true (pps_dbus_document_call_load_fd_sync (
	    session->proxy, g_variant_new_handle (handle), "",
	    G_DBUS_CALL_FLAGS_NONE, 5000, fd_list,
	    &n_pages, NULL, NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpint (n_pages, ==, 1);
	page_info (session);
}

static void
reject_invalid_requests (void)
{
	g_autoptr (PpsDocumentSession) session = new_session ();
	g_autoptr (GError) error = NULL;
	g_auto (GStrv) interfaces = NULL;

	g_assert_false (pps_dbus_document_call_create_sync (
	    session->proxy, "application/pdf", G_DBUS_CALL_FLAGS_NONE, 5000,
	    &interfaces, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
	g_clear_error (&error);
	g_assert_false (pps_dbus_document_call_get_page_info_sync (
	    session->proxy, 0, G_DBUS_CALL_FLAGS_NONE, 5000,
	    NULL, NULL, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	g_assert_false (pps_dbus_document_call_get_form_text_sync (
	    session->proxy, 1, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
	g_clear_error (&error);
	g_assert_false (pps_dbus_document_call_remove_annotation_sync (
	    session->proxy, 1, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
	g_clear_error (&error);
	g_assert_false (pps_dbus_document_call_get_attachments_sync (
	    session->proxy, 0, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
	g_clear_error (&error);
	g_assert_cmpint (load (session, "missing.pdf", "", &error), ==, -1);
	g_assert_nonnull (error);
	g_clear_error (&error);
	g_assert_cmpint (load (session, "utf16le-annot.pdf", "", &error), ==, 1);
	g_assert_no_error (error);
	g_assert_false (pps_dbus_document_call_get_attachments_sync (
	    session->proxy, 1, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	g_assert_false (pps_dbus_document_call_get_form_text_sync (
	    session->proxy, 0, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	for (int page = -1; page <= 1; page += 2) {
		g_assert_false (pps_dbus_document_call_get_page_info_sync (
		    session->proxy, page, G_DBUS_CALL_FLAGS_NONE, 5000,
		    NULL, NULL, NULL, NULL, &error));
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
		g_clear_error (&error);
	}
	g_assert_cmpint (load (session, "PasswordEncrypted.pdf", "password", &error), ==, -1);
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
	g_clear_error (&error);
	page_info (session);
}

static void
worker_failure_is_independent (void)
{
	g_autoptr (PpsDocumentSession) first = new_session ();
	g_autoptr (PpsDocumentSession) second = new_session ();
	g_autoptr (GError) error = NULL;

	g_assert_cmpint (load (second, "utf16le-annot.pdf", "", &error), ==, 1);
	g_assert_no_error (error);
	g_subprocess_force_exit (first->process);
	g_assert_true (g_subprocess_wait (first->process, NULL, &error));
	g_assert_no_error (error);
	g_assert_false (pps_dbus_document_call_get_page_info_sync (
	    first->proxy, 0, G_DBUS_CALL_FLAGS_NONE, 5000,
	    NULL, NULL, NULL, NULL, &error));
	g_assert_nonnull (error);
	page_info (second);
}

static void
disconnect_stops_worker (void)
{
	g_autoptr (PpsDocumentSession) session = new_session ();
	g_autoptr (GError) error = NULL;

	g_assert_true (g_dbus_connection_close_sync (session->connection, NULL, &error));
	g_assert_no_error (error);
	g_assert_true (g_subprocess_wait_check (session->process, NULL, &error));
	g_assert_no_error (error);
}

static void
cancel_startup (void)
{
	g_autoptr (GCancellable) cancellable = g_cancellable_new ();
	g_autoptr (GError) error = NULL;
	g_autoptr (PpsDocumentSession) session = NULL;

	g_cancellable_cancel (cancellable);
	session = _pps_document_session_new (cancellable, &error);
	g_assert_null (session);
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void
render_pages (void)
{
	g_autoptr (PpsDocumentSession) session = new_session ();
	g_autoptr (PpsPage) page = pps_page_new (0);
	g_autoptr (PpsRenderContext) context = pps_render_context_new (page, 0, .5, PPS_RENDER_ANNOTS_ALL);
	g_autoptr (GError) error = NULL;
	double width, height;

	g_assert_cmpint (load (session, "utf16le-annot.pdf", "", &error), ==, 1);
	g_assert_no_error (error);
	g_assert_true (pps_dbus_document_call_get_page_info_sync (
	    session->proxy, 0, G_DBUS_CALL_FLAGS_NONE, 5000,
	    &width, &height, NULL, NULL, &error));
	g_assert_no_error (error);
	for (int rotation = 0; rotation < 360; rotation += 90) {
		context->rotation = rotation;
		for (int thumbnail = 0; thumbnail <= 1; thumbnail++) {
			cairo_surface_t *surface = _pps_document_session_render (session, context, thumbnail, NULL, &error);
			gboolean flipped = rotation == 90 || rotation == 270;
			g_assert_no_error (error);
			g_assert_nonnull (surface);
			g_assert_cmpint (cairo_image_surface_get_width (surface), ==,
			                 (int) ((flipped ? height : width) * .5 + .5));
			g_assert_cmpint (cairo_image_surface_get_height (surface), ==,
			                 (int) ((flipped ? width : height) * .5 + .5));
			cairo_surface_destroy (surface);
		}
	}
	/* Explicit target sizes are independent of the scale factor. */
	context->scale = 0;
	pps_render_context_set_target_size (context, 160, 200);
	cairo_surface_t *surface = _pps_document_session_render (session, context, FALSE, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpint (cairo_image_surface_get_width (surface), ==, 160);
	g_assert_cmpint (cairo_image_surface_get_height (surface), ==, 200);
	unsigned char *pixels = cairo_image_surface_get_data (surface);
	gsize size = cairo_image_surface_get_stride (surface) * 200;
	gboolean nonuniform = FALSE;
	for (gsize i = 4; i < size; i += 4) {
		if (memcmp (pixels, pixels + i, 4) != 0) {
			nonuniform = TRUE;
			break;
		}
	}
	g_assert_true (nonuniform);
	cairo_surface_destroy (surface);

	pps_render_context_set_target_size (context, -1, -1);
	context->scale = .5;
	context->rotation = 45;
	g_assert_null (_pps_document_session_render (session, context, FALSE, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	context->rotation = 0;
	context->scale = NAN;
	g_assert_null (_pps_document_session_render (session, context, FALSE, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	context->scale = 1e100;
	g_assert_null (_pps_document_session_render (session, context, FALSE, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	context->scale = .5;
	surface = _pps_document_session_render (session, context, FALSE, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (surface);
	cairo_surface_destroy (surface);
}

typedef struct {
	GMainLoop *loop;
	GError *error;
} PendingRequest;

static void
render_finished (GObject *source, GAsyncResult *result, gpointer data)
{
	PendingRequest *request = data;
	g_autoptr (GVariant) buffer = NULL;
	g_autoptr (GUnixFDList) descriptors = NULL;

	g_assert_false (pps_dbus_document_call_render_finish (
	    PPS_DBUS_DOCUMENT (source), &buffer, &descriptors, result, &request->error));
	g_main_loop_quit (request->loop);
}

static void
pending_request (gconstpointer data)
{
	gboolean crash = GPOINTER_TO_INT (data);
	g_autoptr (PpsDocumentSession) first = new_session ();
	g_autoptr (PpsDocumentSession) second = new_session ();
	g_autoptr (GCancellable) cancellable = g_cancellable_new ();
	g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
	g_autoptr (GError) error = NULL;
	PendingRequest request = { loop, NULL };
	siginfo_t status;
	pid_t pid = g_ascii_strtoll (g_subprocess_get_identifier (first->process), NULL, 10);

	g_assert_cmpint (load (first, "utf16le-annot.pdf", "", &error), ==, 1);
	g_assert_no_error (error);
	g_assert_cmpint (load (second, "utf16le-annot.pdf", "", &error), ==, 1);
	g_assert_no_error (error);
	g_subprocess_send_signal (first->process, SIGSTOP);
	g_assert_cmpint (waitid (P_PID, pid, &status, WSTOPPED | WNOWAIT), ==, 0);
	g_assert_cmpint (status.si_code, ==, CLD_STOPPED);
	pps_dbus_document_call_render (
	    first->proxy, 0, 0, .5, -1, -1, PPS_RENDER_ANNOTS_ALL, FALSE,
	    G_DBUS_CALL_FLAGS_NONE, 5000, NULL, cancellable, render_finished, &request);
	/* A suspended worker cannot prevent another document from responding. */
	page_info (second);
	if (crash)
		g_subprocess_force_exit (first->process);
	else
		g_cancellable_cancel (cancellable);
	g_main_loop_run (loop);
	g_assert_nonnull (request.error);
	if (!crash) {
		g_assert_error (request.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
		g_subprocess_send_signal (first->process, SIGCONT);
		page_info (first);
	}
	g_clear_error (&request.error);
	page_info (second);
}

static void
startup_failure (gconstpointer data)
{
	if (!g_test_subprocess ()) {
		g_test_trap_subprocess (NULL, 5 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_DEFAULT);
		g_test_trap_assert_passed ();
		return;
	}
	g_autoptr (GError) error = NULL;
	g_setenv ("PPS_DOCUMENT_WORKER", data, TRUE);
	g_autoptr (PpsDocumentSession) session = _pps_document_session_new (NULL, &error);
	g_assert_null (session);
	g_assert_nonnull (error);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/document-worker/load-and-password", load_and_password);
	g_test_add_func ("/document-worker/load-fd", load_fd);
	g_test_add_func ("/document-worker/invalid-requests", reject_invalid_requests);
	g_test_add_func ("/document-worker/independent-failure", worker_failure_is_independent);
	g_test_add_func ("/document-worker/disconnect", disconnect_stops_worker);
	g_test_add_func ("/document-worker/cancel-startup", cancel_startup);
	g_test_add_func ("/document-worker/render", render_pages);
	g_test_add_data_func ("/document-worker/pending-crash", GINT_TO_POINTER (TRUE), pending_request);
	g_test_add_data_func ("/document-worker/pending-cancel", GINT_TO_POINTER (FALSE), pending_request);
	g_test_add_data_func ("/document-worker/startup-missing", "/nonexistent/papers-test-worker", startup_failure);
	g_test_add_data_func ("/document-worker/startup-no-handshake", "/usr/bin/true", startup_failure);
	return g_test_run ();
}

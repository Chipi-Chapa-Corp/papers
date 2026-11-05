// SPDX-License-Identifier: GPL-2.0-or-later

#include <fcntl.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

#include "pps-document-session-private.h"
#include "pps-document.h"

static PpsDocumentSession *
new_session (void)
{
	g_autoptr (GError) error = NULL;
	g_auto (GStrv) interfaces = NULL;
	PpsDocumentSession *session = _pps_document_session_new (NULL, &error);

	g_assert_no_error (error);
	g_assert_nonnull (session);
	g_assert_true (pps_dbus_document_call_create_sync (
	    session->proxy, "application/pdf", G_DBUS_CALL_FLAGS_NONE, 5000,
	    &interfaces, NULL, &error));
	g_assert_no_error (error);
	g_assert_true (g_strv_contains ((const char *const *) interfaces, "PpsDocumentText"));
	g_assert_true (g_strv_contains ((const char *const *) interfaces, "PpsDocumentSecurity"));
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
	double width, height;

	g_assert_true (pps_dbus_document_call_get_page_info_sync (
	    session->proxy, 0, G_DBUS_CALL_FLAGS_NONE, 5000,
	    &width, &height, &label, NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpfloat (width, >, 0);
	g_assert_cmpfloat (height, >, 0);
	g_assert_nonnull (label);
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
	    &interfaces, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
	g_clear_error (&error);
	g_assert_false (pps_dbus_document_call_get_page_info_sync (
	    session->proxy, 0, G_DBUS_CALL_FLAGS_NONE, 5000,
	    NULL, NULL, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	g_assert_cmpint (load (session, "missing.pdf", "", &error), ==, -1);
	g_assert_nonnull (error);
	g_clear_error (&error);
	g_assert_cmpint (load (session, "utf16le-annot.pdf", "", &error), ==, 1);
	g_assert_no_error (error);
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
	return g_test_run ();
}

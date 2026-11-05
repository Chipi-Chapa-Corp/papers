// SPDX-License-Identifier: GPL-2.0-or-later

#include <fcntl.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include "pps-document-factory-private.h"
#include "pps-document-proxy-private.h"
#include "pps-document-session-private.h"
#include "pps-document-signature-private.h"
#include "pps-document-signatures.h"
#include "pps-document-text.h"
#include "pps-init.h"

static char *fixture_dir;
static char *source_path;
static char *certutil;

static char *
password_callback (const char *name, gpointer data)
{
	guint *requests = data;
	g_assert_nonnull (name);
	(*requests)++;
	return g_strdup ("papers-test-password");
}

static PpsDocument *
open_document_full (const char *path, gboolean proxy)
{
	g_autoptr (GError) error = NULL;
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	PpsDocument *document = proxy ? _pps_document_proxy_new ("application/pdf", NULL, &error) : _pps_document_factory_create_document ("application/pdf", &error);
	g_assert_no_error (error);
	g_assert_true (pps_document_load (document, uri, &error));
	g_assert_no_error (error);
	pps_document_setup_cache (document);
	return document;
}

static PpsDocument *
open_document (const char *path)
{
	return open_document_full (path, FALSE);
}

typedef struct {
	GMainLoop *loop;
	GError *error;
	gboolean succeeded;
} SignResult;

static void
sign_finished (GObject *source, GAsyncResult *result, gpointer data)
{
	SignResult *state = data;
	state->succeeded = pps_document_signatures_sign_finish (PPS_DOCUMENT_SIGNATURES (source), result, &state->error);
	g_main_loop_quit (state->loop);
}

static gboolean
sign_timed_out (gpointer data)
{
	g_error ("Document signing did not finish");
	return G_SOURCE_REMOVE;
}

static gboolean
sign_document (PpsDocument *document, PpsSignature *signature, GCancellable *cancellable, GError **error)
{
	g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
	SignResult state = { .loop = loop };
	guint timeout = g_timeout_add_seconds (10, sign_timed_out, NULL);
	g_assert_true (pps_document_signatures_sign (PPS_DOCUMENT_SIGNATURES (document), signature, cancellable, sign_finished, &state));
	g_main_loop_run (loop);
	g_source_remove (timeout);
	if (state.error)
		g_propagate_error (error, state.error);
	return state.succeeded;
}

static PpsSignature *
new_signature (PpsCertificateInfo *certificate, const char *path)
{
	PpsSignature *signature = pps_signature_new (PPS_SIGNATURE_STATUS_INVALID, certificate);
	PpsRectangle rect = { 10, 10, 190, 90 };
	pps_signature_set_rect (signature, &rect);
	pps_signature_set_page (signature, 0);
	pps_signature_set_signature (signature, "Papers signature test");
	pps_signature_set_signature_left (signature, "Signer");
	pps_signature_set_destination_file (signature, path);
	pps_signature_set_password (signature, "papers-test-password");
	return signature;
}

static PpsCertificateInfo *
get_certificate (PpsDocument *document, guint *requests)
{
	pps_document_signatures_set_password_callback (PPS_DOCUMENT_SIGNATURES (document), password_callback, requests);
	g_autolist (PpsCertificateInfo) certificates = pps_document_signatures_get_available_signing_certificates (PPS_DOCUMENT_SIGNATURES (document));
	g_assert_cmpuint (g_list_length (certificates), ==, 1);
	g_autofree char *id = NULL;
	g_object_get (certificates->data, "id", &id, NULL);
	g_assert_nonnull (id);
	PpsCertificateInfo *certificate = pps_document_signatures_get_certificate_info (PPS_DOCUMENT_SIGNATURES (document), id);
	g_assert_nonnull (certificate);
	g_assert_null (pps_document_signatures_get_certificate_info (PPS_DOCUMENT_SIGNATURES (document), "missing-certificate"));
	return certificate;
}

static char *
create_signed_pdf (const char *name)
{
	g_autoptr (PpsDocument) document = open_document (source_path);
	guint requests = 0;
	g_autoptr (PpsCertificateInfo) certificate = get_certificate (document, &requests);
	char *path = g_build_filename (fixture_dir, name, NULL);
	g_autoptr (PpsSignature) signature = new_signature (certificate, path);
	g_autoptr (GError) error = NULL;
	g_assert_true (sign_document (document, signature, NULL, &error));
	g_assert_no_error (error);
	g_assert_false (pps_document_signatures_has_signatures (PPS_DOCUMENT_SIGNATURES (document)));
	pps_document_signatures_set_password_callback (PPS_DOCUMENT_SIGNATURES (document), NULL, NULL);
	return path;
}

static void
sign_and_verify (void)
{
	if (!certutil) {
		g_test_skip ("certutil is needed to create a temporary signing certificate");
		return;
	}
	g_autoptr (PpsDocument) document = open_document (source_path);
	g_assert_true (pps_document_signatures_can_sign (PPS_DOCUMENT_SIGNATURES (document)));
	g_assert_false (pps_document_signatures_has_signatures (PPS_DOCUMENT_SIGNATURES (document)));
	g_assert_null (pps_document_signatures_get_signatures (PPS_DOCUMENT_SIGNATURES (document)));
	g_autofree char *path = create_signed_pdf ("signed.pdf");
	g_autoptr (PpsDocument) signed_document = open_document (path);
	g_assert_true (pps_document_signatures_has_signatures (PPS_DOCUMENT_SIGNATURES (signed_document)));
	g_autolist (PpsSignature) signatures = pps_document_signatures_get_signatures (PPS_DOCUMENT_SIGNATURES (signed_document));
	g_assert_cmpuint (g_list_length (signatures), ==, 1);
	g_autoptr (PpsCertificateInfo) signer = NULL;
	g_autoptr (GDateTime) time = NULL;
	int signature_status, certificate_status;
	g_object_get (signatures->data, "certificate-info", &signer, "signature-time", &time, "status", &signature_status, NULL);
	g_object_get (signer, "status", &certificate_status, NULL);
	g_test_message ("Signature status %d, certificate status %d", signature_status, certificate_status);
	g_assert_true (pps_signature_is_valid (signatures->data));
	g_assert_nonnull (signer);
	g_assert_nonnull (time);
	g_autofree char *name = NULL;
	g_object_get (signer, "subject-common-name", &name, NULL);
	g_assert_cmpstr (name, ==, "Papers Test Signer");
	pps_document_signatures_set_password_callback (PPS_DOCUMENT_SIGNATURES (document), NULL, NULL);
}

static void
check_worker_signatures (const char *path, int expected_status)
{
	g_autoptr (PpsDocument) document = open_document (path);
	g_autolist (PpsSignature) native = pps_document_signatures_get_signatures (PPS_DOCUMENT_SIGNATURES (document));
	g_autoptr (PpsDocumentSession) session = _pps_document_session_new (NULL, NULL);
	g_assert_nonnull (session);
	g_autoptr (GError) error = NULL;
	g_assert_true (pps_dbus_document_call_create_sync (session->proxy, "application/pdf", G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL, &error));
	g_assert_no_error (error);
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	g_assert_true (pps_dbus_document_call_load_sync (session->proxy, uri, "", G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, &error));
	g_assert_no_error (error);
	g_autoptr (GVariant) metadata = NULL, signatures = NULL;
	g_assert_true (pps_dbus_document_call_get_info_sync (session->proxy, G_DBUS_CALL_FLAGS_NONE, 5000, &metadata, NULL, NULL, NULL, NULL, NULL, &error));
	g_assert_no_error (error);
	gboolean has_signatures, can_sign;
	g_assert_true (g_variant_lookup (metadata, "has-signatures", "b", &has_signatures));
	g_assert_true (g_variant_lookup (metadata, "can-sign", "b", &can_sign));
	g_assert_true (can_sign);
	g_assert_cmpint (has_signatures, ==, expected_status >= 0);
	g_assert_true (pps_dbus_document_call_get_signatures_sync (session->proxy, G_DBUS_CALL_FLAGS_NONE, 10000, &signatures, NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpuint (g_variant_n_children (signatures), ==, g_list_length (native));
	g_assert_cmpuint (g_variant_n_children (signatures), ==, expected_status >= 0 ? 1 : 0);
	if (native) {
		g_autoptr (GVariant) expected = g_variant_ref_sink (_pps_signature_info_to_variant (native->data));
		g_autoptr (GVariant) value = g_variant_get_child_value (signatures, 0);
		g_assert_true (g_variant_equal (value, expected));
		g_autoptr (PpsSignature) copy = _pps_signature_info_from_variant (value, &error);
		g_assert_no_error (error);
		g_assert_nonnull (copy);
		int status;
		g_object_get (copy, "status", &status, NULL);
		g_assert_cmpint (status, ==, expected_status);
		g_assert_cmpint (pps_signature_is_valid (copy), ==, expected_status == PPS_SIGNATURE_STATUS_VALID);
		g_clear_object (&document);
		g_clear_pointer (&session, _pps_document_session_free);
		g_autoptr (GVariant) again = g_variant_ref_sink (_pps_signature_info_to_variant (copy));
		g_assert_true (g_variant_equal (again, expected));
	}
}

static void
worker_signatures (void)
{
	if (!certutil) {
		g_test_skip ("certutil is needed to create a temporary signing certificate");
		return;
	}
	g_autofree char *path = create_signed_pdf ("worker-signed.pdf");
	check_worker_signatures (path, PPS_SIGNATURE_STATUS_VALID);
	g_autofree char *contents = NULL;
	gsize length;
	g_autoptr (GError) error = NULL;
	g_assert_true (g_file_get_contents (path, &contents, &length, &error));
	g_assert_no_error (error);
	/* Both line endings are valid PDF syntax, but the signed bytes differ. */
	g_assert_cmpuint (length, >, 8);
	g_assert_true (g_str_has_prefix (contents, "%PDF-"));
	g_assert_cmpint (contents[8], ==, '\n');
	contents[8] = '\r';
	g_autofree char *tampered = g_build_filename (fixture_dir, "tampered.pdf", NULL);
	g_assert_true (g_file_set_contents (tampered, contents, length, &error));
	g_assert_no_error (error);
	check_worker_signatures (tampered, PPS_SIGNATURE_STATUS_DIGEST_MISMATCH);
}

static void
worker_unsigned (void)
{
	check_worker_signatures (source_path, -1);
}

static void
signing_errors (gconstpointer data)
{
	if (!certutil) {
		g_test_skip ("certutil is needed to create a temporary signing certificate");
		return;
	}
	g_autoptr (PpsDocument) document = open_document_full (source_path, GPOINTER_TO_INT (data));
	guint requests = 0;
	g_autoptr (PpsCertificateInfo) certificate = get_certificate (document, &requests);
	g_autofree char *path = g_build_filename (fixture_dir, "missing-directory", "signed.pdf", NULL);
	g_autoptr (PpsSignature) signature = new_signature (certificate, path);
	g_autoptr (GError) error = NULL;
	g_assert_false (sign_document (document, signature, NULL, &error));
	g_assert_nonnull (error);
	g_clear_error (&error);
	g_autoptr (GCancellable) cancellable = g_cancellable_new ();
	g_cancellable_cancel (cancellable);
	g_assert_false (sign_document (document, signature, cancellable, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_clear_error (&error);
	if (GPOINTER_TO_INT (data)) {
		g_autofree char *existing = g_build_filename (fixture_dir, "existing-output.pdf", NULL);
		g_assert_true (g_file_set_contents (existing, "Original contents", -1, &error));
		g_assert_no_error (error);
		g_autoptr (PpsCertificateInfo) missing = g_object_new (PPS_TYPE_CERTIFICATE_INFO, "id", "missing-certificate", NULL);
		g_autoptr (PpsSignature) invalid = new_signature (missing, existing);
		g_assert_false (sign_document (document, invalid, NULL, &error));
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
		g_clear_error (&error);
		g_autofree char *contents = NULL;
		g_assert_true (g_file_get_contents (existing, &contents, NULL, &error));
		g_assert_no_error (error);
		g_assert_cmpstr (contents, ==, "Original contents");
	}
	pps_document_signatures_set_password_callback (PPS_DOCUMENT_SIGNATURES (document), NULL, NULL);
}

static void
invalid_signing_inputs (void)
{
	g_autoptr (PpsDocument) document = open_document (source_path);
	g_autoptr (PpsCertificateInfo) missing = g_object_new (PPS_TYPE_CERTIFICATE_INFO, "id", "missing-certificate", NULL);
	g_autofree char *path = g_build_filename (fixture_dir, "invalid.pdf", NULL);
	for (guint i = 0; i < 6; i++) {
		g_autoptr (PpsSignature) signature = new_signature (i == 0 ? NULL : missing, path);
		if (i == 1)
			pps_signature_set_destination_file (signature, NULL);
		if (i == 2) {
			g_clear_object (&signature);
			signature = pps_signature_new (PPS_SIGNATURE_STATUS_INVALID, missing);
			pps_signature_set_destination_file (signature, path);
		}
		if (i == 3)
			pps_signature_set_page (signature, 1);
		if (i == 4)
			pps_signature_set_page (signature, G_MAXUINT);
		g_autoptr (GError) error = NULL;
		g_assert_false (sign_document (document, signature, NULL, &error));
		GIOErrorEnum expected = i == 5 ? G_IO_ERROR_NOT_FOUND : G_IO_ERROR_INVALID_ARGUMENT;
		g_assert_error (error, G_IO_ERROR, expected);
		g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
	}
}

typedef struct {
	GMainLoop *loop;
	GVariant *value;
	GError *error;
} WorkerResult;

static void
worker_call_finished (GObject *source, GAsyncResult *result, gpointer data)
{
	WorkerResult *state = data;
	state->value = g_dbus_proxy_call_with_unix_fd_list_finish (G_DBUS_PROXY (source), NULL, result, &state->error);
	g_main_loop_quit (state->loop);
}

static GVariant *
call_worker (PpsDocumentSession *session, const char *method, GVariant *parameters, GUnixFDList *descriptors, GError **error)
{
	g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
	WorkerResult state = { .loop = loop };
	g_dbus_proxy_call_with_unix_fd_list (G_DBUS_PROXY (session->proxy), method, parameters, G_DBUS_CALL_FLAGS_NONE, 10000,
	                                     descriptors, NULL, worker_call_finished, &state);
	g_main_loop_run (loop);
	if (state.error)
		g_propagate_error (error, state.error);
	return state.value;
}

typedef struct {
	guint requests;
	GCancellable *cancel;
} PasswordState;

static gboolean
handle_password (PpsDBusSignaturePasswords *interface, GDBusMethodInvocation *invocation, const char *name, PasswordState *state)
{
	g_assert_true (g_main_context_is_owner (g_main_context_default ()));
	g_assert_nonnull (name);
	state->requests++;
	if (state->cancel)
		g_cancellable_cancel (state->cancel);
	pps_dbus_signature_passwords_complete_request_password (interface, invocation, TRUE, "papers-test-password");
	return TRUE;
}

static PpsDocumentSession *
new_worker (void)
{
	g_autoptr (GError) error = NULL;
	PpsDocumentSession *session = _pps_document_session_new (NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (session);
	g_assert_true (pps_dbus_document_call_create_sync (session->proxy, "application/pdf", G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL, &error));
	g_assert_no_error (error);
	g_autofree char *uri = g_filename_to_uri (source_path, NULL, NULL);
	g_assert_true (pps_dbus_document_call_load_sync (session->proxy, uri, "", G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, &error));
	g_assert_no_error (error);
	return session;
}

static PpsDBusSignaturePasswords *
register_passwords (PpsDocumentSession *session, PasswordState *state)
{
	PpsDBusSignaturePasswords *interface = pps_dbus_signature_passwords_skeleton_new ();
	g_signal_connect (interface, "handle-request-password", G_CALLBACK (handle_password), state);
	g_autoptr (GError) error = NULL;
	g_assert_true (g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (interface), session->connection, PPS_SIGNATURE_PASSWORD_PATH, &error));
	g_assert_no_error (error);
	return interface;
}

static void
worker_certificates (void)
{
	if (!certutil) {
		g_test_skip ("certutil is needed to create a temporary signing certificate");
		return;
	}
	g_autoptr (PpsDocumentSession) session = new_worker ();
	PasswordState state = { 0 };
	g_autoptr (PpsDBusSignaturePasswords) passwords = register_passwords (session, &state);
	g_autoptr (GError) error = NULL;
	g_autoptr (GVariant) result = call_worker (session, "GetSigningCertificates", NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (result);
	g_assert_cmpuint (state.requests, >, 0);
	g_autoptr (GVariant) certificates = g_variant_get_child_value (result, 0);
	g_assert_cmpuint (g_variant_n_children (certificates), ==, 1);
	g_autoptr (GVariant) first = g_variant_get_child_value (certificates, 0);
	g_autoptr (PpsCertificateInfo) certificate = _pps_certificate_info_from_variant (first, &error);
	g_assert_no_error (error);
	g_assert_nonnull (certificate);
	g_autofree char *id = NULL;
	g_object_get (certificate, "id", &id, NULL);
	g_autoptr (GVariant) info = call_worker (session, "GetCertificateInfo", g_variant_new ("(s)", id), NULL, &error);
	g_assert_no_error (error);
	gboolean present;
	g_autoptr (GVariant) value = NULL;
	g_variant_get (info, "(b@a{sv})", &present, &value);
	g_assert_true (present);
	g_assert_true (g_variant_equal (first, value));
	g_clear_pointer (&info, g_variant_unref);
	g_clear_pointer (&value, g_variant_unref);
	info = call_worker (session, "GetCertificateInfo", g_variant_new ("(s)", "missing-certificate"), NULL, &error);
	g_assert_no_error (error);
	g_variant_get (info, "(b@a{sv})", &present, &value);
	g_assert_false (present);
	g_assert_cmpuint (g_variant_n_children (value), ==, 0);
	g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (passwords));
}

static void
worker_sign (gconstpointer data)
{
	if (!certutil) {
		g_test_skip ("certutil is needed to create a temporary signing certificate");
		return;
	}
	gboolean cancel = GPOINTER_TO_INT (data);
	g_autoptr (PpsDocument) document = open_document (source_path);
	guint requests = 0;
	g_autoptr (PpsCertificateInfo) certificate = get_certificate (document, &requests);
	pps_document_signatures_set_password_callback (PPS_DOCUMENT_SIGNATURES (document), NULL, NULL);
	g_autoptr (PpsSignature) signature = new_signature (certificate, "/unused-output-path");
	g_autoptr (GVariant) options = g_variant_ref_sink (_pps_signature_signing_data_to_variant (signature));
	g_autoptr (PpsDocumentSession) session = new_worker ();
	g_autoptr (GCancellable) cancellable = g_cancellable_new ();
	PasswordState state = { .cancel = cancel ? cancellable : NULL };
	g_autoptr (PpsDBusSignaturePasswords) passwords = register_passwords (session, &state);
	g_autoptr (GError) error = NULL;
	g_autofree char *path = NULL;
	int fd = g_file_open_tmp ("papers-signed-XXXXXX", &path, &error);
	g_assert_no_error (error);
	g_assert_cmpint (fd, >=, 0);
	g_assert_cmpint (g_unlink (path), ==, 0);
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	int output_handle = g_unix_fd_list_append (descriptors, fd, &error);
	g_assert_no_error (error);
	GPollFD pollfd;
	g_assert_true (g_cancellable_make_pollfd (cancellable, &pollfd));
	int cancel_handle = g_unix_fd_list_append (descriptors, pollfd.fd, &error);
	g_assert_no_error (error);
	g_autoptr (GVariant) result = call_worker (session, "Sign", g_variant_new ("(hh@a{sv})", output_handle, cancel_handle, options), descriptors, &error);
	g_assert_cmpuint (state.requests, >, 0);
	if (cancel) {
		g_assert_null (result);
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
		g_clear_error (&error);
	} else {
		g_assert_no_error (error);
		g_assert_nonnull (result);
		g_assert_cmpint (lseek (fd, 0, SEEK_END), >, 0);
		g_autoptr (PpsDocument) signed_document = _pps_document_factory_create_document ("application/pdf", &error);
		g_assert_no_error (error);
		g_assert_true (pps_document_load_fd (signed_document, dup (fd), &error));
		g_assert_no_error (error);
		g_autolist (PpsSignature) signatures = pps_document_signatures_get_signatures (PPS_DOCUMENT_SIGNATURES (signed_document));
		g_assert_cmpuint (g_list_length (signatures), ==, 1);
		g_assert_true (pps_signature_is_valid (signatures->data));
	}
	g_clear_pointer (&result, g_variant_unref);
	result = call_worker (session, "GetSigningCertificates", NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (result);
	g_cancellable_release_fd (cancellable);
	g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (passwords));
	close (fd);
}

static void
worker_signing_inputs (void)
{
	g_autoptr (PpsDocumentSession) session = new_worker ();
	g_autoptr (PpsSignature) signature = new_signature (NULL, NULL);
	g_autoptr (GVariant) options = g_variant_ref_sink (_pps_signature_signing_data_to_variant (signature));
	g_autoptr (GCancellable) cancellable = g_cancellable_new ();
	g_autoptr (GError) error = NULL;
	g_autofree char *path = NULL;
	int fd = g_file_open_tmp ("papers-signing-inputs-XXXXXX", &path, &error);
	g_assert_no_error (error);
	g_assert_cmpint (fd, >=, 0);
	g_assert_cmpint (g_unlink (path), ==, 0);
	GPollFD pollfd;
	g_assert_true (g_cancellable_make_pollfd (cancellable, &pollfd));
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	g_assert_cmpint (g_unix_fd_list_append (descriptors, fd, NULL), ==, 0);
	g_assert_cmpint (g_unix_fd_list_append (descriptors, pollfd.fd, NULL), ==, 1);
	int read_only = g_open (source_path, O_RDONLY | O_CLOEXEC, 0);
	g_assert_cmpint (read_only, >=, 0);
	g_assert_cmpint (g_unix_fd_list_append (descriptors, read_only, NULL), ==, 2);
	close (read_only);
	int pipes[2];
	g_assert_cmpint (pipe (pipes), ==, 0);
	g_assert_cmpint (g_unix_fd_list_append (descriptors, pipes[1], NULL), ==, 3);
	close (pipes[0]);
	close (pipes[1]);
	const int handles[][2] = { { -1, 1 }, { 0, -1 }, { 9, 1 }, { 0, 9 }, { 2, 1 }, { 3, 1 }, { 1, 0 } };
	for (guint i = 0; i < G_N_ELEMENTS (handles); i++) {
		g_assert_null (call_worker (session, "Sign", g_variant_new ("(hh@a{sv})", handles[i][0], handles[i][1], options), descriptors, &error));
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
		g_clear_error (&error);
	}
	GVariantDict dict;
	g_variant_dict_init (&dict, options);
	g_variant_dict_insert (&dict, "page", "i", 1);
	g_assert_null (call_worker (session, "Sign", g_variant_new ("(hh@a{sv})", 0, 1, g_variant_dict_end (&dict)), descriptors, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	g_assert_null (call_worker (session, "Sign", g_variant_new ("(hh@a{sv})", 0, 1, g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0)), descriptors, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_clear_error (&error);
	/* Missing certificate data reaches a checked native error, never an assert. */
	g_assert_null (call_worker (session, "Sign", g_variant_new ("(hh@a{sv})", 0, 1, options), descriptors, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	g_cancellable_cancel (cancellable);
	g_assert_null (call_worker (session, "Sign", g_variant_new ("(hh@a{sv})", 0, 1, options), descriptors, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_clear_error (&error);
	g_assert_cmpint (lseek (fd, 0, SEEK_END), ==, 0);
	g_autoptr (GVariant) result = call_worker (session, "GetInfo", NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (result);
	g_cancellable_release_fd (cancellable);
	close (fd);
}

typedef struct {
	GThread *thread;
	guint requests;
	gboolean reject;
	gboolean wrong_once;
	GCancellable *cancel;
	PpsDocument *reenter;
} ProxyPassword;

static char *
proxy_password (const char *name, gpointer data)
{
	ProxyPassword *state = data;
	g_assert_true (g_thread_self () == state->thread);
	g_assert_nonnull (name);
	state->requests++;
	if (state->reenter) {
		g_autoptr (PpsPage) page = pps_document_get_page (state->reenter, 0);
		g_autofree char *text = pps_document_text_get_text (PPS_DOCUMENT_TEXT (state->reenter), page);
		g_assert_null (text);
		g_assert_null (pps_document_signatures_get_available_signing_certificates (PPS_DOCUMENT_SIGNATURES (state->reenter)));
	}
	if (state->cancel)
		g_cancellable_cancel (state->cancel);
	if (state->wrong_once && state->requests == 1)
		return g_strdup ("wrong password");
	return state->reject ? NULL : g_strdup ("papers-test-password");
}

static PpsCertificateInfo *
proxy_certificate (PpsDocument *document, ProxyPassword *state)
{
	pps_document_signatures_set_password_callback (PPS_DOCUMENT_SIGNATURES (document), proxy_password, state);
	g_autolist (PpsCertificateInfo) certificates = pps_document_signatures_get_available_signing_certificates (PPS_DOCUMENT_SIGNATURES (document));
	g_assert_cmpuint (g_list_length (certificates), ==, 1);
	g_autofree char *id = NULL;
	g_object_get (certificates->data, "id", &id, NULL);
	g_assert_nonnull (id);
	PpsCertificateInfo *certificate = pps_document_signatures_get_certificate_info (PPS_DOCUMENT_SIGNATURES (document), id);
	g_assert_nonnull (certificate);
	g_assert_null (pps_document_signatures_get_certificate_info (PPS_DOCUMENT_SIGNATURES (document), "missing-certificate"));
	return certificate;
}

static void
proxy_sign_and_verify (void)
{
	if (!certutil) {
		g_test_skip ("certutil is needed to create a temporary signing certificate");
		return;
	}
	/* Start without native oracle types to verify that signing, validation, and
	 * rendering keep the document parser out of the caller. */
	if (!g_test_subprocess ()) {
		g_test_trap_subprocess (NULL, 15 * G_USEC_PER_SEC, 0);
		g_test_trap_assert_passed ();
		return;
	}
	g_assert_cmpuint (g_type_from_name ("PdfDocument"), ==, G_TYPE_INVALID);
	g_autoptr (PpsDocument) document = open_document_full (source_path, TRUE);
	g_assert_true (PPS_IS_DOCUMENT_SIGNATURES (document));
	g_assert_true (pps_document_signatures_can_sign (PPS_DOCUMENT_SIGNATURES (document)));
	g_assert_false (pps_document_signatures_has_signatures (PPS_DOCUMENT_SIGNATURES (document)));
	ProxyPassword state = { .thread = g_thread_self () };
	g_autoptr (PpsCertificateInfo) certificate = proxy_certificate (document, &state);
	g_assert_cmpuint (state.requests, >, 0);
	g_autofree char *path = g_build_filename (fixture_dir, "proxy-signed.pdf", NULL);
	g_autofree char *old_contents = g_malloc0 (1024 * 1024);
	g_autoptr (GError) error = NULL;
	g_assert_true (g_file_set_contents (path, old_contents, 1024 * 1024, &error));
	g_assert_no_error (error);
	g_autoptr (PpsSignature) signature = new_signature (certificate, path);
	GdkRGBA font = { 0, 0, 1, 1 }, border = { 1, 0, 0, 1 }, background = { 0, 1, 0, 1 };
	pps_signature_set_font_color (signature, &font);
	pps_signature_set_border_color (signature, &border);
	pps_signature_set_background_color (signature, &background);
	pps_signature_set_font_size (signature, 14);
	pps_signature_set_left_font_size (signature, 24);
	pps_signature_set_border_width (signature, 2);
	g_assert_true (sign_document (document, signature, NULL, &error));
	g_assert_no_error (error);
	GStatBuf statbuf;
	g_assert_cmpint (g_stat (path, &statbuf), ==, 0);
	g_assert_cmpint (statbuf.st_size, <, 1024 * 1024);
	g_assert_false (pps_document_signatures_has_signatures (PPS_DOCUMENT_SIGNATURES (document)));
	g_autoptr (PpsDocument) signed_document = open_document_full (path, TRUE);
	g_assert_true (pps_document_signatures_has_signatures (PPS_DOCUMENT_SIGNATURES (signed_document)));
	g_autolist (PpsSignature) signatures = pps_document_signatures_get_signatures (PPS_DOCUMENT_SIGNATURES (signed_document));
	g_assert_cmpuint (g_list_length (signatures), ==, 1);
	g_assert_true (pps_signature_is_valid (signatures->data));
	g_autoptr (PpsPage) page = pps_document_get_page (signed_document, 0);
	g_autofree char *text = pps_document_text_get_text (PPS_DOCUMENT_TEXT (signed_document), page);
	g_assert_nonnull (text);
	g_assert_nonnull (strstr (text, "Signature fixture"));
	g_autoptr (PpsRenderContext) context = pps_render_context_new (page, 0, 1, PPS_RENDER_ANNOTS_ALL);
	cairo_surface_t *surface = pps_document_render (signed_document, context);
	g_assert_nonnull (surface);
	guint red = 0, green = 0, blue = 0;
	for (int y = 0; y < cairo_image_surface_get_height (surface); y++) {
		const guint32 *pixels = (const guint32 *) (cairo_image_surface_get_data (surface) + y * cairo_image_surface_get_stride (surface));
		for (int x = 0; x < cairo_image_surface_get_width (surface); x++) {
			guint32 rgb = pixels[x] & 0xffffff;
			red += rgb == 0xff0000;
			green += rgb == 0x00ff00;
			blue += rgb == 0x0000ff;
		}
	}
	cairo_surface_destroy (surface);
	g_assert_cmpuint (red, >, 100);
	g_assert_cmpuint (green, >, 1000);
	g_assert_cmpuint (blue, >, 10);
	g_clear_object (&document);
	g_clear_object (&signed_document);
	g_assert_true (pps_signature_is_valid (signatures->data));
	g_assert_cmpuint (g_type_from_name ("PdfDocument"), ==, G_TYPE_INVALID);
}

static void
proxy_certificate_callbacks (void)
{
	if (!certutil) {
		g_test_skip ("certutil is needed to create a temporary signing certificate");
		return;
	}
	g_autoptr (PpsDocument) document = open_document_full (source_path, TRUE);
	g_assert_null (pps_document_signatures_get_available_signing_certificates (PPS_DOCUMENT_SIGNATURES (document)));
	ProxyPassword denied = { .thread = g_thread_self (), .reject = TRUE };
	pps_document_signatures_set_password_callback (PPS_DOCUMENT_SIGNATURES (document), proxy_password, &denied);
	g_assert_null (pps_document_signatures_get_available_signing_certificates (PPS_DOCUMENT_SIGNATURES (document)));
	g_assert_cmpuint (denied.requests, >, 0);
	guint previous = denied.requests;
	ProxyPassword accepted = { .thread = g_thread_self (), .wrong_once = TRUE, .reenter = document };
	pps_document_signatures_set_password_callback (PPS_DOCUMENT_SIGNATURES (document), proxy_password, &accepted);
	g_autolist (PpsCertificateInfo) first_attempt = pps_document_signatures_get_available_signing_certificates (PPS_DOCUMENT_SIGNATURES (document));
	g_autoptr (PpsCertificateInfo) certificate = proxy_certificate (document, &accepted);
	g_assert_cmpuint (accepted.requests, >=, 2);
	g_assert_cmpuint (denied.requests, ==, previous);
	GWeakRef weak;
	g_weak_ref_init (&weak, document);
	g_clear_object (&document);
	g_assert_null (g_weak_ref_get (&weak));
	g_weak_ref_clear (&weak);
}

typedef struct {
	PpsDocument *document;
	GMainLoop *loop;
	GList *certificates;
} BackgroundCertificates;

static gboolean
quit_loop (gpointer data)
{
	g_main_loop_quit (data);
	return G_SOURCE_REMOVE;
}

static gpointer
load_certificates (gpointer data)
{
	BackgroundCertificates *state = data;
	state->certificates = pps_document_signatures_get_available_signing_certificates (PPS_DOCUMENT_SIGNATURES (state->document));
	g_idle_add (quit_loop, state->loop);
	return NULL;
}

static void
proxy_background_certificates (void)
{
	if (!certutil) {
		g_test_skip ("certutil is needed to create a temporary signing certificate");
		return;
	}
	g_autoptr (PpsDocument) document = open_document_full (source_path, TRUE);
	ProxyPassword password = { .thread = g_thread_self () };
	pps_document_signatures_set_password_callback (PPS_DOCUMENT_SIGNATURES (document), proxy_password, &password);
	g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
	BackgroundCertificates state = { document, loop, NULL };
	guint timeout = g_timeout_add_seconds (10, sign_timed_out, NULL);
	GThread *thread = g_thread_new ("signing-certificates", load_certificates, &state);
	g_main_loop_run (loop);
	g_thread_join (thread);
	g_source_remove (timeout);
	g_assert_cmpuint (password.requests, >, 0);
	g_assert_cmpuint (g_list_length (state.certificates), ==, 1);
	g_list_free_full (state.certificates, g_object_unref);
}

static void
proxy_cancel_sign (void)
{
	if (!certutil) {
		g_test_skip ("certutil is needed to create a temporary signing certificate");
		return;
	}
	g_autoptr (PpsDocument) selector = open_document_full (source_path, TRUE);
	ProxyPassword selection = { .thread = g_thread_self () };
	g_autoptr (PpsCertificateInfo) certificate = proxy_certificate (selector, &selection);
	guint selection_requests = selection.requests;
	g_autoptr (PpsDocument) document = open_document_full (source_path, TRUE);
	g_autoptr (GCancellable) cancellable = g_cancellable_new ();
	ProxyPassword password = { .thread = g_thread_self (), .cancel = cancellable };
	pps_document_signatures_set_password_callback (PPS_DOCUMENT_SIGNATURES (document), proxy_password, &password);
	g_autofree char *path = g_build_filename (fixture_dir, "proxy-cancelled.pdf", NULL);
	g_autoptr (PpsSignature) signature = new_signature (certificate, path);
	g_autoptr (GError) error = NULL;
	g_assert_false (sign_document (document, signature, cancellable, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_assert_cmpuint (password.requests, >, 0);
	g_assert_cmpuint (selection.requests, ==, selection_requests);
	g_clear_error (&error);
	password.cancel = NULL;
	g_cancellable_reset (cancellable);
	g_assert_true (sign_document (document, signature, cancellable, &error));
	g_assert_no_error (error);
	g_autoptr (PpsDocument) signed_document = open_document_full (path, TRUE);
	g_autolist (PpsSignature) signatures = pps_document_signatures_get_signatures (PPS_DOCUMENT_SIGNATURES (signed_document));
	g_assert_cmpuint (g_list_length (signatures), ==, 1);
	g_assert_true (pps_signature_is_valid (signatures->data));
}

static void
proxy_worker_failure (void)
{
	if (!certutil) {
		g_test_skip ("certutil is needed to create a temporary signing certificate");
		return;
	}
	g_autoptr (PpsDocument) document = open_document_full (source_path, TRUE);
	ProxyPassword password = { .thread = g_thread_self () };
	g_autoptr (PpsCertificateInfo) certificate = proxy_certificate (document, &password);
	PpsDocumentSession *session = _pps_document_proxy_get_session (document);
	g_subprocess_force_exit (session->process);
	g_assert_true (g_subprocess_wait (session->process, NULL, NULL));
	g_autofree char *path = g_build_filename (fixture_dir, "failed-worker.pdf", NULL);
	g_autoptr (PpsSignature) signature = new_signature (certificate, path);
	g_autoptr (GError) error = NULL;
	g_assert_false (sign_document (document, signature, NULL, &error));
	g_assert_nonnull (error);
	g_clear_error (&error);
	g_autoptr (PpsDocument) other = open_document_full (source_path, TRUE);
	ProxyPassword other_password = { .thread = g_thread_self () };
	pps_document_signatures_set_password_callback (PPS_DOCUMENT_SIGNATURES (other), proxy_password, &other_password);
	g_autofree char *other_path = g_build_filename (fixture_dir, "other-worker.pdf", NULL);
	pps_signature_set_destination_file (signature, other_path);
	g_assert_true (sign_document (other, signature, NULL, &error));
	g_assert_no_error (error);
	g_autoptr (PpsDocument) signed_document = open_document_full (other_path, TRUE);
	g_autolist (PpsSignature) signatures = pps_document_signatures_get_signatures (PPS_DOCUMENT_SIGNATURES (signed_document));
	g_assert_cmpuint (g_list_length (signatures), ==, 1);
	g_assert_true (pps_signature_is_valid (signatures->data));
}

static void
run_command (const char *const *argv)
{
	g_autoptr (GError) error = NULL;
	g_autoptr (GSubprocess) process = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
	g_assert_no_error (error);
	g_autofree char *stderr_text = NULL;
	g_assert_true (g_subprocess_communicate_utf8 (process, NULL, NULL, NULL, &stderr_text, &error));
	g_assert_no_error (error);
	if (!g_subprocess_get_successful (process))
		g_error ("Certificate fixture creation failed: %s", stderr_text);
}

static void
create_fixtures (void)
{
	g_autoptr (GError) error = NULL;
	fixture_dir = g_dir_make_tmp ("papers-signatures-XXXXXX", &error);
	g_assert_no_error (error);
	g_setenv ("HOME", fixture_dir, TRUE);
	g_setenv ("XDG_CONFIG_HOME", fixture_dir, TRUE);
	g_setenv ("POPPLER_SIGNATURE_BACKEND", "NSS", TRUE);
	source_path = g_build_filename (fixture_dir, "source.pdf", NULL);
	/* A vector fixture needs no font initialization in the proxy's caller. */
	const char *objects[] = {
		"<< /Type /Catalog /Pages 2 0 R >>",
		"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
		"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
		"<< /Length 48 >>\nstream\nBT /F1 12 Tf 20 80 Td (Signature fixture) Tj ET\nendstream",
		"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
	};
	g_autoptr (GString) pdf = g_string_new ("%PDF-1.7\n");
	gsize offsets[G_N_ELEMENTS (objects)];
	for (guint i = 0; i < G_N_ELEMENTS (objects); i++) {
		offsets[i] = pdf->len;
		g_string_append_printf (pdf, "%u 0 obj\n%s\nendobj\n", i + 1, objects[i]);
	}
	gsize xref = pdf->len;
	g_string_append_printf (pdf, "xref\n0 %u\n0000000000 65535 f \n", (guint) G_N_ELEMENTS (objects) + 1);
	for (guint i = 0; i < G_N_ELEMENTS (objects); i++)
		g_string_append_printf (pdf, "%010" G_GSIZE_FORMAT " 00000 n \n", offsets[i]);
	g_string_append_printf (pdf, "trailer\n<< /Size %u /Root 1 0 R >>\nstartxref\n%" G_GSIZE_FORMAT "\n%%%%EOF\n", (guint) G_N_ELEMENTS (objects) + 1, xref);
	g_assert_true (g_file_set_contents (source_path, pdf->str, pdf->len, &error));
	g_assert_no_error (error);
	certutil = g_find_program_in_path ("certutil");
	if (!certutil)
		return;
	g_autofree char *directory = g_build_filename (fixture_dir, ".mozilla", "firefox", "test.default", NULL);
	g_assert_cmpint (g_mkdir_with_parents (directory, 0700), ==, 0);
	g_autofree char *database = g_strconcat ("sql:", directory, NULL);
	g_autofree char *password = g_build_filename (fixture_dir, "password", NULL);
	g_assert_true (g_file_set_contents (password, "papers-test-password", -1, &error));
	g_assert_no_error (error);
	g_autofree char *noise = g_build_filename (fixture_dir, "noise", NULL);
	guint32 random[32];
	for (guint i = 0; i < G_N_ELEMENTS (random); i++)
		random[i] = g_random_int ();
	g_assert_true (g_file_set_contents (noise, (const char *) random, sizeof random, &error));
	g_assert_no_error (error);
	const char *init[] = { certutil, "-N", "-d", database, "-f", password, NULL };
	run_command (init);
	const char *generate[] = { certutil, "-S", "-x", "-n", "Papers Test Signer", "-s", "CN=Papers Test Signer,O=Papers Tests,E=signer@example.org",
		                   "-t", "P,P,P", "-k", "rsa", "-g", "2048", "-m", "1", "-v", "120", "-z", noise,
		                   "--keyUsage", "digitalSignature", "-d", database, "-f", password, NULL };
	run_command (generate);
}

static void
remove_fixture (const char *path)
{
	if (g_file_test (path, G_FILE_TEST_IS_DIR)) {
		g_autoptr (GDir) directory = g_dir_open (path, 0, NULL);
		const char *name;
		while ((name = g_dir_read_name (directory))) {
			g_autofree char *child = g_build_filename (path, name, NULL);
			remove_fixture (child);
		}
		g_assert_cmpint (g_rmdir (path), ==, 0);
	} else {
		g_assert_cmpint (g_unlink (path), ==, 0);
	}
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	create_fixtures ();
	pps_init ();
	g_test_add_func ("/document-signatures/sign-and-verify", sign_and_verify);
	g_test_add_data_func ("/document-signatures/errors", GINT_TO_POINTER (FALSE), signing_errors);
	g_test_add_func ("/document-signatures/invalid-inputs", invalid_signing_inputs);
	g_test_add_func ("/document-signatures/worker-signed-and-tampered", worker_signatures);
	g_test_add_func ("/document-signatures/worker-unsigned", worker_unsigned);
	g_test_add_func ("/document-signatures/worker-certificates", worker_certificates);
	g_test_add_data_func ("/document-signatures/worker-sign", GINT_TO_POINTER (FALSE), worker_sign);
	g_test_add_data_func ("/document-signatures/worker-cancel-sign", GINT_TO_POINTER (TRUE), worker_sign);
	g_test_add_func ("/document-signatures/worker-signing-inputs", worker_signing_inputs);
	g_test_add_func ("/document-signatures/proxy-sign-and-verify", proxy_sign_and_verify);
	g_test_add_func ("/document-signatures/proxy-certificate-callbacks", proxy_certificate_callbacks);
	g_test_add_func ("/document-signatures/proxy-background-certificates", proxy_background_certificates);
	g_test_add_func ("/document-signatures/proxy-cancel-sign", proxy_cancel_sign);
	g_test_add_data_func ("/document-signatures/proxy-errors", GINT_TO_POINTER (TRUE), signing_errors);
	g_test_add_func ("/document-signatures/proxy-worker-failure", proxy_worker_failure);
	int result = g_test_run ();
	pps_shutdown ();
	remove_fixture (fixture_dir);
	g_free (fixture_dir);
	g_free (source_path);
	g_free (certutil);
	return result;
}

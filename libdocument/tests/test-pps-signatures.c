// SPDX-License-Identifier: GPL-2.0-or-later

#include <cairo-pdf.h>
#include <glib/gstdio.h>

#include "pps-document-factory-private.h"
#include "pps-document-session-private.h"
#include "pps-document-signature-private.h"
#include "pps-document-signatures.h"
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
open_document (const char *path)
{
	g_autoptr (GError) error = NULL;
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	PpsDocument *document = _pps_document_factory_create_document ("application/pdf", &error);
	g_assert_no_error (error);
	g_assert_true (pps_document_load (document, uri, &error));
	g_assert_no_error (error);
	pps_document_setup_cache (document);
	return document;
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
signing_errors (void)
{
	if (!certutil) {
		g_test_skip ("certutil is needed to create a temporary signing certificate");
		return;
	}
	g_autoptr (PpsDocument) document = open_document (source_path);
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
	cairo_surface_t *surface = cairo_pdf_surface_create (source_path, 200, 200);
	cairo_t *cr = cairo_create (surface);
	cairo_move_to (cr, 20, 120);
	cairo_show_text (cr, "Signature fixture");
	cairo_show_page (cr);
	cairo_destroy (cr);
	cairo_surface_destroy (surface);
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
	g_test_add_func ("/document-signatures/errors", signing_errors);
	g_test_add_func ("/document-signatures/invalid-inputs", invalid_signing_inputs);
	g_test_add_func ("/document-signatures/worker-signed-and-tampered", worker_signatures);
	g_test_add_func ("/document-signatures/worker-unsigned", worker_unsigned);
	int result = g_test_run ();
	pps_shutdown ();
	remove_fixture (fixture_dir);
	g_free (fixture_dir);
	g_free (source_path);
	g_free (certutil);
	return result;
}

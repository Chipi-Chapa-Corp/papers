// SPDX-License-Identifier: GPL-2.0-or-later

#include <fcntl.h>
#include <glib-unix.h>
#include <sys/stat.h>

#include "pps-document-signature-private.h"
#include "pps-document-signatures.h"
#include "pps-document-worker-private.h"

static char *
request_password (const char *name, gpointer data)
{
	g_autoptr (GVariant) result = g_dbus_connection_call_sync (
	    data, NULL, PPS_SIGNATURE_PASSWORD_PATH, "org.gnome.Papers.SignaturePasswords", "RequestPassword",
	    g_variant_new ("(s)", name ? name : ""), G_VARIANT_TYPE ("(bs)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
	if (!result)
		return NULL;
	gboolean present;
	const char *password;
	g_variant_get (result, "(b&s)", &present, &password);
	return present ? g_strdup (password) : NULL;
}

static void
set_password_callback (Worker *worker, GDBusMethodInvocation *invocation)
{
	pps_document_signatures_set_password_callback (
	    PPS_DOCUMENT_SIGNATURES (worker->document), invocation ? request_password : NULL,
	    invocation ? g_dbus_method_invocation_get_connection (invocation) : NULL);
}

static gboolean
get_signatures (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, Worker *worker)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_SIGNATURES, invocation))
		return TRUE;
	set_password_callback (worker, invocation);
	g_autolist (PpsSignature) signatures = pps_document_signatures_get_signatures (PPS_DOCUMENT_SIGNATURES (worker->document));
	set_password_callback (worker, NULL);
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));
	for (GList *item = signatures; item; item = item->next)
		g_variant_builder_add_value (&builder, _pps_signature_info_to_variant (item->data));
	pps_dbus_document_complete_get_signatures (interface, invocation, g_variant_builder_end (&builder));
	return TRUE;
}

static gboolean
get_certificates (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, Worker *worker)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_SIGNATURES, invocation))
		return TRUE;
	set_password_callback (worker, invocation);
	g_autolist (PpsCertificateInfo) certificates = pps_document_signatures_get_available_signing_certificates (PPS_DOCUMENT_SIGNATURES (worker->document));
	set_password_callback (worker, NULL);
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));
	for (GList *item = certificates; item; item = item->next)
		g_variant_builder_add_value (&builder, _pps_certificate_info_to_variant (item->data));
	pps_dbus_document_complete_get_signing_certificates (interface, invocation, g_variant_builder_end (&builder));
	return TRUE;
}

static gboolean
get_certificate (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, const char *id, Worker *worker)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_SIGNATURES, invocation))
		return TRUE;
	set_password_callback (worker, invocation);
	g_autoptr (PpsCertificateInfo) certificate = pps_document_signatures_get_certificate_info (PPS_DOCUMENT_SIGNATURES (worker->document), id);
	set_password_callback (worker, NULL);
	pps_dbus_document_complete_get_certificate_info (interface, invocation, certificate != NULL,
	                                                 certificate ? _pps_certificate_info_to_variant (certificate) : g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0));
	return TRUE;
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
cancel_sign (int fd, GIOCondition condition, gpointer data)
{
	g_cancellable_cancel (data);
	return G_SOURCE_REMOVE;
}

static gboolean
invalid_descriptor (GDBusMethodInvocation *invocation)
{
	g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid signing file descriptor");
	return TRUE;
}

static gboolean
sign_document (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, GUnixFDList *descriptors, GVariant *output_handle, GVariant *cancel_handle, GVariant *data, Worker *worker)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_SIGNATURES, invocation))
		return TRUE;
	g_autoptr (GError) error = NULL;
	g_autoptr (PpsSignature) signature = _pps_signature_signing_data_from_variant (data, &error);
	if (!signature) {
		g_dbus_method_invocation_return_gerror (invocation, error);
		return TRUE;
	}
	int output_index = g_variant_get_handle (output_handle), cancel_index = g_variant_get_handle (cancel_handle);
	if (!descriptors || output_index < 0 || cancel_index < 0 ||
	    output_index >= g_unix_fd_list_get_length (descriptors) || cancel_index >= g_unix_fd_list_get_length (descriptors))
		return invalid_descriptor (invocation);
	const int *fds = g_unix_fd_list_peek_fds (descriptors, NULL);
	struct stat statbuf;
	int flags = fcntl (fds[output_index], F_GETFL);
	if (fstat (fds[output_index], &statbuf) == -1 || !S_ISREG (statbuf.st_mode) || flags == -1 || (flags & O_ACCMODE) != O_RDWR)
		return invalid_descriptor (invocation);
	if (!pps_document_worker_check_page (worker, pps_signature_get_page (signature), PPS_TYPE_DOCUMENT_SIGNATURES, invocation))
		return TRUE;
	PpsDocumentSignatures *document = PPS_DOCUMENT_SIGNATURES (worker->document);
	if (!pps_document_signatures_can_sign (document) || !PPS_DOCUMENT_SIGNATURES_GET_IFACE (document)->sign || !PPS_DOCUMENT_SIGNATURES_GET_IFACE (document)->sign_finish) {
		g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "The backend cannot sign documents");
		return TRUE;
	}
	g_autofree char *path = g_strdup_printf ("/proc/self/fd/%d", fds[output_index]);
	pps_signature_set_destination_file (signature, path);
	g_autoptr (GCancellable) cancellable = g_cancellable_new ();
	GPollFD pollfd = { fds[cancel_index], G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, 0 };
	if (g_poll (&pollfd, 1, 0) > 0)
		g_cancellable_cancel (cancellable);
	if (g_cancellable_set_error_if_cancelled (cancellable, &error)) {
		g_dbus_method_invocation_return_gerror (invocation, error);
		return TRUE;
	}
	/* A private context services cancellation and native async completion without
	 * dispatching another document operation during signing. */
	g_autoptr (GMainContext) context = g_main_context_new ();
	g_autoptr (GMainLoop) loop = g_main_loop_new (context, FALSE);
	g_autoptr (GSource) source = g_unix_fd_source_new (fds[cancel_index], G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL);
	g_source_set_priority (source, G_PRIORITY_HIGH);
	g_source_set_callback (source, G_SOURCE_FUNC (cancel_sign), cancellable, NULL);
	g_source_attach (source, context);
	SignResult state = { .loop = loop };
	g_main_context_push_thread_default (context);
	set_password_callback (worker, invocation);
	pps_document_signatures_sign (document, signature, cancellable, sign_finished, &state);
	g_main_loop_run (loop);
	set_password_callback (worker, NULL);
	g_main_context_pop_thread_default (context);
	g_source_destroy (source);
	if (state.error) {
		g_dbus_method_invocation_return_gerror (invocation, state.error);
		g_error_free (state.error);
	} else if (!state.succeeded) {
		g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to sign document");
	} else {
		pps_dbus_document_complete_sign (interface, invocation, NULL);
	}
	return TRUE;
}

void
pps_document_worker_bind_signatures (Worker *worker, PpsDBusDocument *interface)
{
	g_signal_connect (interface, "handle-get-signatures", G_CALLBACK (get_signatures), worker);
	g_signal_connect (interface, "handle-get-signing-certificates", G_CALLBACK (get_certificates), worker);
	g_signal_connect (interface, "handle-get-certificate-info", G_CALLBACK (get_certificate), worker);
	g_signal_connect (interface, "handle-sign", G_CALLBACK (sign_document), worker);
}

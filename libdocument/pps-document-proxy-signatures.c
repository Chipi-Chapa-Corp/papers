// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include "pps-document-call-private.h"
#include "pps-document-proxy-private.h"
#include "pps-document-signature-private.h"

static const char *password_key = "pps-proxy-signature-passwords";

typedef struct {
	PpsSignaturePasswordCallback callback;
	gpointer data;
	GMainContext *context;
	GThread *thread;
	GWeakRef proxy;
} PasswordCallback;

static void
free_callback (gpointer data)
{
	PasswordCallback *state = data;
	g_main_context_unref (state->context);
	g_thread_unref (state->thread);
	g_weak_ref_clear (&state->proxy);
	g_free (state);
}

static void
unexport_passwords (gpointer data)
{
	g_dbus_interface_skeleton_unexport (data);
	g_object_unref (data);
}

static gboolean
request_password (PpsDBusSignaturePasswords *interface, GDBusMethodInvocation *invocation, const char *name, PasswordCallback *state)
{
	/* The callback can run a nested loop and close or move its document. */
	g_autoptr (PpsDBusSignaturePasswords) hold = g_object_ref (interface);
	g_autoptr (GObject) proxy = g_weak_ref_get (&state->proxy);
	if (proxy)
		g_object_set_data (proxy, PPS_DOCUMENT_PASSWORD_PENDING, GINT_TO_POINTER (TRUE));
	g_autofree char *password = state->callback (name, state->data);
	if (proxy)
		g_object_set_data (proxy, PPS_DOCUMENT_PASSWORD_PENDING, NULL);
	pps_dbus_signature_passwords_complete_request_password (interface, invocation, password != NULL, password ? password : "");
	return TRUE;
}

static void
set_password_callback (PpsDocumentSignatures *document, PpsSignaturePasswordCallback callback, gpointer data)
{
	g_object_set_data (G_OBJECT (document), password_key, NULL);
	if (!callback)
		return;
	g_autoptr (PpsDBusSignaturePasswords) interface = pps_dbus_signature_passwords_skeleton_new ();
	PasswordCallback *state = g_new0 (PasswordCallback, 1);
	state->callback = callback;
	state->data = data;
	state->context = g_main_context_ref_thread_default ();
	state->thread = g_thread_ref (g_thread_self ());
	g_object_set_data_full (G_OBJECT (interface), password_key, state, free_callback);
	g_signal_connect (interface, "handle-request-password", G_CALLBACK (request_password), state);
	PpsDocumentSession *session = _pps_document_proxy_get_session (PPS_DOCUMENT (document));
	g_weak_ref_init (&state->proxy, session->proxy);
	if (g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (interface), session->connection, PPS_SIGNATURE_PASSWORD_PATH, NULL))
		g_object_set_data_full (G_OBJECT (document), password_key, g_steal_pointer (&interface), unexport_passwords);
}

typedef struct {
	GMainLoop *loop;
	GVariant *value;
	GError *error;
} PasswordCall;

static void
password_call_finished (GObject *source, GAsyncResult *result, gpointer data)
{
	PasswordCall *state = data;
	state->value = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &state->error);
	g_main_loop_quit (state->loop);
}

static gpointer
ref_passwords (gpointer data, gpointer unused)
{
	return data ? g_object_ref (data) : NULL;
}

static GVariant *
call_with_passwords (PpsDocumentSignatures *document, const char *method, GVariant *parameters)
{
	g_autoptr (PpsDocumentSignatures) hold = g_object_ref (document);
	g_autoptr (PpsDBusSignaturePasswords) passwords = g_object_dup_data (G_OBJECT (document), password_key, ref_passwords, NULL);
	PasswordCallback *state = passwords ? g_object_get_data (G_OBJECT (passwords), password_key) : NULL;
	GDBusProxy *proxy = G_DBUS_PROXY (_pps_document_proxy_get_session (PPS_DOCUMENT (document))->proxy);
	if (g_object_get_data (G_OBJECT (proxy), PPS_DOCUMENT_PASSWORD_PENDING)) {
		if (parameters) {
			g_variant_ref_sink (parameters);
			g_variant_unref (parameters);
		}
		return NULL;
	}
	if (!state || state->thread != g_thread_self ())
		return _pps_document_call_sync (proxy, method, parameters, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
	/* Synchronous certificate access must still dispatch the password prompt
	 * on the thread and context where its UI callback was registered. */
	g_autoptr (GMainLoop) loop = g_main_loop_new (state->context, FALSE);
	PasswordCall call = { .loop = loop };
	g_main_context_push_thread_default (state->context);
	g_dbus_proxy_call (proxy, method, parameters, G_DBUS_CALL_FLAGS_NONE, -1, NULL, password_call_finished, &call);
	g_main_loop_run (loop);
	g_main_context_pop_thread_default (state->context);
	g_clear_error (&call.error);
	return call.value;
}

static GList *
get_certificates (PpsDocumentSignatures *document)
{
	g_autoptr (GVariant) result = call_with_passwords (document, "GetSigningCertificates", NULL);
	if (!result || !g_variant_is_of_type (result, G_VARIANT_TYPE ("(aa{sv})")))
		return NULL;
	g_autoptr (GVariant) certificates = g_variant_get_child_value (result, 0);
	GList *list = NULL;
	for (gsize i = 0; i < g_variant_n_children (certificates); i++) {
		g_autoptr (GVariant) value = g_variant_get_child_value (certificates, i);
		PpsCertificateInfo *certificate = _pps_certificate_info_from_variant (value, NULL);
		if (!certificate) {
			g_list_free_full (list, g_object_unref);
			return NULL;
		}
		list = g_list_prepend (list, certificate);
	}
	return g_list_reverse (list);
}

static PpsCertificateInfo *
get_certificate (PpsDocumentSignatures *document, const char *id)
{
	if (!id || !*id)
		return NULL;
	g_autoptr (GVariant) result = call_with_passwords (document, "GetCertificateInfo", g_variant_new ("(s)", id));
	if (!result || !g_variant_is_of_type (result, G_VARIANT_TYPE ("(ba{sv})")))
		return NULL;
	gboolean present;
	g_autoptr (GVariant) value = NULL;
	g_variant_get (result, "(b@a{sv})", &present, &value);
	return present ? _pps_certificate_info_from_variant (value, NULL) : NULL;
}

static GList *
get_signatures (PpsDocumentSignatures *document)
{
	g_autoptr (GVariant) result = call_with_passwords (document, "GetSignatures", NULL);
	if (!result || !g_variant_is_of_type (result, G_VARIANT_TYPE ("(aa{sv})")))
		return NULL;
	g_autoptr (GVariant) signatures = g_variant_get_child_value (result, 0);
	GList *list = NULL;
	for (gsize i = 0; i < g_variant_n_children (signatures); i++) {
		g_autoptr (GVariant) value = g_variant_get_child_value (signatures, i);
		PpsSignature *signature = _pps_signature_info_from_variant (value, NULL);
		if (!signature) {
			g_list_free_full (list, g_object_unref);
			return NULL;
		}
		list = g_list_prepend (list, signature);
	}
	return g_list_reverse (list);
}

static gboolean
can_sign (PpsDocumentSignatures *document)
{
	gboolean value = FALSE;
	GVariant *metadata = _pps_document_proxy_get_metadata (PPS_DOCUMENT (document));
	return metadata && g_variant_lookup (metadata, "can-sign", "b", &value) && value;
}

static gboolean
has_signatures (PpsDocumentSignatures *document)
{
	gboolean value = FALSE;
	GVariant *metadata = _pps_document_proxy_get_metadata (PPS_DOCUMENT (document));
	return metadata && g_variant_lookup (metadata, "has-signatures", "b", &value) && value;
}

static void
release_cancellable (gpointer data)
{
	g_cancellable_release_fd (data);
	g_object_unref (data);
}

static void
sign_ready (GObject *source, GAsyncResult *result, gpointer data)
{
	g_autoptr (GTask) task = data;
	g_autoptr (GError) error = NULL;
	if (!pps_dbus_document_call_sign_finish (PPS_DBUS_DOCUMENT (source), NULL, result, &error))
		g_task_return_error (task, g_steal_pointer (&error));
	else
		g_task_return_boolean (task, TRUE);
}

static void
sign_document (PpsDocumentSignatures *document, PpsSignature *signature, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
	g_autoptr (GTask) task = g_task_new (document, cancellable, callback, data);
	if (g_task_return_error_if_cancelled (task))
		return;
	g_autoptr (GVariant) options = g_variant_ref_sink (_pps_signature_signing_data_to_variant (signature));
	g_autoptr (PpsSignature) checked = _pps_signature_signing_data_from_variant (options, NULL);
	g_autoptr (PpsCertificateInfo) certificate = NULL;
	g_object_get (signature, "certificate-info", &certificate, NULL);
	if (!checked || !certificate || pps_signature_get_page (signature) >= pps_document_get_n_pages (PPS_DOCUMENT (document))) {
		g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid document signing data");
		return;
	}
	const char *path = pps_signature_get_destination_file (signature);
	if (!path || !*path) {
		g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Missing signing output filename");
		return;
	}
	int fd = g_open (path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (fd == -1) {
		int saved_errno = errno;
		g_task_return_new_error (task, G_IO_ERROR, g_io_error_from_errno (saved_errno), "%s", g_strerror (saved_errno));
		return;
	}
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	g_autoptr (GError) error = NULL;
	int output_handle = g_unix_fd_list_append (descriptors, fd, &error);
	close (fd);
	if (output_handle == -1) {
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}
	g_autoptr (GCancellable) operation = cancellable ? g_object_ref (cancellable) : g_cancellable_new ();
	GPollFD pollfd;
	if (!g_cancellable_make_pollfd (operation, &pollfd)) {
		g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not create signing cancellation descriptor");
		return;
	}
	g_task_set_task_data (task, g_steal_pointer (&operation), release_cancellable);
	int cancel_handle = g_unix_fd_list_append (descriptors, pollfd.fd, &error);
	if (cancel_handle == -1) {
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}
	PpsDocumentSession *session = _pps_document_proxy_get_session (PPS_DOCUMENT (document));
	/* Wait for the worker to acknowledge cancellation before releasing its
	 * descriptor or allowing a caller to reset and reuse the cancellable. */
	pps_dbus_document_call_sign (session->proxy, g_variant_new_handle (output_handle), g_variant_new_handle (cancel_handle),
	                             options, G_DBUS_CALL_FLAGS_NONE, -1,
	                             descriptors, NULL, sign_ready, g_steal_pointer (&task));
}

static gboolean
sign_finish (PpsDocumentSignatures *document, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail (g_task_is_valid (result, document), FALSE);
	return g_task_propagate_boolean (G_TASK (result), error);
}

void
_pps_document_proxy_signatures_iface_init (PpsDocumentSignaturesInterface *iface)
{
	iface->set_password_callback = set_password_callback;
	iface->get_available_signing_certificates = get_certificates;
	iface->get_certificate_info = get_certificate;
	iface->get_signatures = get_signatures;
	iface->can_sign = can_sign;
	iface->has_signatures = has_signatures;
	iface->sign = sign_document;
	iface->sign_finish = sign_finish;
}

// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-call-private.h"

typedef struct {
	GMainLoop *loop;
	GVariant *result;
	GUnixFDList *fds;
	GError *error;
} DocumentCall;

static void
call_finished (GObject *source, GAsyncResult *result, gpointer data)
{
	DocumentCall *call = data;
	call->result = g_dbus_proxy_call_with_unix_fd_list_finish (G_DBUS_PROXY (source), &call->fds, result, &call->error);
	g_main_loop_quit (call->loop);
}

GVariant *
_pps_document_call_with_fds_sync (GDBusProxy *proxy, const char *method, GVariant *parameters, GDBusCallFlags flags, int timeout, GUnixFDList *fds, GUnixFDList **out_fds, GCancellable *cancellable, GError **error)
{
	if (out_fds)
		*out_fds = NULL;
	if (g_object_get_data (G_OBJECT (proxy), PPS_DOCUMENT_PASSWORD_PENDING)) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PENDING, "The document is waiting for a password");
		/* Match GDBus' ownership of floating method parameters on failure. */
		if (parameters) {
			g_variant_ref_sink (parameters);
			g_variant_unref (parameters);
		}
		return NULL;
	}
	if (!cancellable)
		cancellable = g_cancellable_get_current ();
	GMainContext *context = g_main_context_get_thread_default ();
	if (!context)
		context = g_main_context_default ();
	if (!g_main_context_is_owner (context))
		return g_dbus_proxy_call_with_unix_fd_list_sync (proxy, method, parameters, flags, timeout, fds, out_fds, cancellable, error);

	/* Keep the UI servicing events while a synchronous document API waits for
	 * its worker. Background jobs use the ordinary cancellable GIO call. */
	g_autoptr (GDBusProxy) hold = g_object_ref (proxy);
	g_autoptr (GMainLoop) loop = g_main_loop_new (context, FALSE);
	DocumentCall call = { .loop = loop };
	g_main_context_push_thread_default (context);
	g_dbus_proxy_call_with_unix_fd_list (proxy, method, parameters, flags, timeout, fds, cancellable, call_finished, &call);
	g_main_loop_run (loop);
	g_main_context_pop_thread_default (context);
	if (out_fds)
		*out_fds = g_steal_pointer (&call.fds);
	g_clear_object (&call.fds);
	if (call.error)
		g_propagate_error (error, call.error);
	return call.result;
}

GVariant *
_pps_document_call_sync (GDBusProxy *proxy, const char *method, GVariant *parameters, GDBusCallFlags flags, int timeout, GCancellable *cancellable, GError **error)
{
	return _pps_document_call_with_fds_sync (proxy, method, parameters, flags, timeout, NULL, NULL, cancellable, error);
}

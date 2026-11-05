// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include "pps-document-buffer-private.h"
#include "pps-document-session-private.h"

void
_pps_document_session_free (PpsDocumentSession *session)
{
	if (!session)
		return;

	g_clear_object (&session->proxy);
	if (session->connection)
		g_dbus_connection_close_sync (session->connection, NULL, NULL);
	/* Also stop a backend that cannot service the connection close. */
	if (session->process)
		g_subprocess_force_exit (session->process);
	g_clear_object (&session->connection);
	g_clear_object (&session->process);
	g_free (session);
}

PpsDocumentSession *
_pps_document_session_new (GCancellable *cancellable,
                           GError **error)
{
	g_autoptr (PpsDocumentSession) session = g_new0 (PpsDocumentSession, 1);
	g_autoptr (GSubprocessLauncher) launcher = NULL;
	g_autoptr (GSocket) socket = NULL;
	g_autoptr (GSocketConnection) stream = NULL;
	const char *worker = g_getenv ("PPS_DOCUMENT_WORKER");
	int sockets[2];

	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		return NULL;

	if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == -1) {
		int saved_errno = errno;
		g_set_error_literal (error, G_IO_ERROR,
		                     g_io_error_from_errno (saved_errno),
		                     g_strerror (saved_errno));
		return NULL;
	}

	launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
	g_subprocess_launcher_take_fd (launcher, sockets[1], PPS_DOCUMENT_WORKER_FD);
	socket = g_socket_new_from_fd (sockets[0], error);
	if (!socket) {
		close (sockets[0]);
		return NULL;
	}
	stream = g_socket_connection_factory_create_connection (socket);
	if (!worker)
		worker = PPS_DOCUMENT_WORKER;
	session->process = g_subprocess_launcher_spawn (launcher, error, worker, NULL);
	g_subprocess_launcher_close (launcher);
	if (!session->process)
		return NULL;

	session->connection = g_dbus_connection_new_sync (
	    G_IO_STREAM (stream), NULL,
	    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
	    NULL, cancellable, error);
	if (!session->connection)
		return NULL;

	session->proxy = pps_dbus_document_proxy_new_sync (
	    session->connection,
	    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
		G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
	    NULL, PPS_DOCUMENT_WORKER_PATH, cancellable, error);
	if (!session->proxy)
		return NULL;

	return g_steal_pointer (&session);
}

cairo_surface_t *
_pps_document_session_render (PpsDocumentSession *session,
                              PpsRenderContext *context,
                              gboolean thumbnail,
                              GCancellable *cancellable,
                              GError **error)
{
	g_autoptr (GVariant) buffer = NULL;
	g_autoptr (GUnixFDList) descriptors = NULL;

	if (!pps_dbus_document_call_render_sync (
		session->proxy, context->page->index, context->rotation,
		context->scale, context->target_width, context->target_height,
		context->annot_flags, thumbnail,
		G_DBUS_CALL_FLAGS_NONE, -1, NULL, &buffer, &descriptors,
		cancellable, error))
		return NULL;
	return _pps_document_surface_from_buffer (buffer, descriptors, error);
}

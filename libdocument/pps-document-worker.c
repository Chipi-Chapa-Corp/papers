// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <gio/gunixfdlist.h>
#include <papers-document.h>

#include "pps-document-factory-private.h"
#include "pps-document-session-private.h"
#include "pps-document-signatures.h"

typedef struct {
	PpsDocument *document;
	gboolean loaded;
} Worker;

static gboolean
create_document (PpsDBusDocument *interface,
                 GDBusMethodInvocation *invocation,
                 const char *mime_type,
                 Worker *worker)
{
	g_autoptr (GError) error = NULL;
	g_autoptr (GPtrArray) interfaces = g_ptr_array_new ();
	GType supported[] = {
		PPS_TYPE_DOCUMENT_ANNOTATIONS, PPS_TYPE_DOCUMENT_ATTACHMENTS,
		PPS_TYPE_DOCUMENT_FIND, PPS_TYPE_DOCUMENT_FONTS,
		PPS_TYPE_DOCUMENT_FORMS, PPS_TYPE_DOCUMENT_IMAGES,
		PPS_TYPE_DOCUMENT_LAYERS, PPS_TYPE_DOCUMENT_LINKS,
		PPS_TYPE_DOCUMENT_MEDIA, PPS_TYPE_DOCUMENT_PRINT,
		PPS_TYPE_DOCUMENT_SECURITY, PPS_TYPE_DOCUMENT_SIGNATURES,
		PPS_TYPE_DOCUMENT_TEXT, PPS_TYPE_DOCUMENT_TRANSITION,
		PPS_TYPE_FILE_EXPORTER, PPS_TYPE_SELECTION
	};

	if (worker->document) {
		g_dbus_method_invocation_return_error_literal (
		    invocation, G_IO_ERROR, G_IO_ERROR_EXISTS,
		    "The worker already owns a document");
		return TRUE;
	}

	worker->document = _pps_document_factory_create_document (mime_type, &error);
	if (!worker->document) {
		g_dbus_method_invocation_return_gerror (invocation, error);
		return TRUE;
	}

	for (guint i = 0; i < G_N_ELEMENTS (supported); i++) {
		if (g_type_is_a (G_OBJECT_TYPE (worker->document), supported[i]))
			g_ptr_array_add (interfaces, (gpointer) g_type_name (supported[i]));
	}
	g_ptr_array_add (interfaces, NULL);
	pps_dbus_document_complete_create (interface, invocation,
	                                   (const char *const *) interfaces->pdata);
	return TRUE;
}

static gboolean
prepare_load (Worker *worker,
              const char *password,
              GDBusMethodInvocation *invocation)
{
	if (!worker->document) {
		g_dbus_method_invocation_return_error_literal (
		    invocation, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
		    "The worker has no document");
		return FALSE;
	}
	if (worker->loaded) {
		g_dbus_method_invocation_return_error_literal (
		    invocation, G_IO_ERROR, G_IO_ERROR_EXISTS,
		    "The worker already loaded its document");
		return FALSE;
	}
	if (password[0] && PPS_IS_DOCUMENT_SECURITY (worker->document))
		pps_document_security_set_password (PPS_DOCUMENT_SECURITY (worker->document),
		                                    password);
	return TRUE;
}

static gboolean
load_document (PpsDBusDocument *interface,
               GDBusMethodInvocation *invocation,
               const char *uri,
               const char *password,
               Worker *worker)
{
	g_autoptr (GError) error = NULL;

	if (!prepare_load (worker, password, invocation))
		return TRUE;
	worker->loaded = pps_document_load (worker->document, uri, &error);
	if (!worker->loaded)
		g_dbus_method_invocation_return_gerror (invocation, error);
	else {
		pps_document_setup_cache (worker->document);
		pps_dbus_document_complete_load (interface, invocation,
		                                 pps_document_get_n_pages (worker->document));
	}
	return TRUE;
}

static gboolean
load_document_fd (PpsDBusDocument *interface,
                  GDBusMethodInvocation *invocation,
                  GUnixFDList *fd_list,
                  GVariant *handle,
                  const char *password,
                  Worker *worker)
{
	g_autoptr (GError) error = NULL;
	int fd;

	if (!prepare_load (worker, password, invocation))
		return TRUE;
	if (!fd_list || g_variant_get_handle (handle) < 0 ||
	    g_variant_get_handle (handle) >= g_unix_fd_list_get_length (fd_list)) {
		g_dbus_method_invocation_return_error_literal (
		    invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		    "Invalid document file descriptor");
		return TRUE;
	}
	fd = g_unix_fd_list_get (fd_list, g_variant_get_handle (handle), &error);
	if (fd != -1)
		worker->loaded = pps_document_load_fd (worker->document, fd, &error);
	if (!worker->loaded)
		g_dbus_method_invocation_return_gerror (invocation, error);
	else {
		pps_document_setup_cache (worker->document);
		pps_dbus_document_complete_load_fd (interface, invocation, NULL,
		                                    pps_document_get_n_pages (worker->document));
	}
	return TRUE;
}

static gboolean
get_page_info (PpsDBusDocument *interface,
               GDBusMethodInvocation *invocation,
               int page,
               Worker *worker)
{
	g_autofree char *label = NULL;
	double width, height;

	if (!worker->loaded || page < 0 ||
	    page >= pps_document_get_n_pages (worker->document)) {
		g_dbus_method_invocation_return_error_literal (
		    invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		    "Page is outside the loaded document");
		return TRUE;
	}
	pps_document_get_page_size (worker->document, page, &width, &height);
	label = pps_document_get_page_label (worker->document, page);
	pps_dbus_document_complete_get_page_info (interface, invocation,
	                                          width, height, label ? label : "");
	return TRUE;
}

static void
connection_closed (GDBusConnection *connection,
                   gboolean remote_peer_vanished,
                   GError *error,
                   GMainLoop *loop)
{
	g_main_loop_quit (loop);
}

int
main (int argc, char **argv)
{
	g_autoptr (GError) error = NULL;
	g_autoptr (GSocket) socket = NULL;
	g_autoptr (GSocketConnection) stream = NULL;
	g_autoptr (GDBusConnection) connection = NULL;
	g_autoptr (PpsDBusDocument) interface = NULL;
	g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
	g_autofree char *guid = g_dbus_generate_guid ();
	Worker worker = { 0 };

	g_set_prgname ("papers-document-worker");
	if (argc != 1)
		return 1;
	socket = g_socket_new_from_fd (PPS_DOCUMENT_WORKER_FD, &error);
	if (!socket)
		goto failed;
	stream = g_socket_connection_factory_create_connection (socket);
	connection = g_dbus_connection_new_sync (
	    G_IO_STREAM (stream), guid,
	    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_SERVER |
		G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING,
	    NULL, NULL, &error);
	if (!connection)
		goto failed;
	if (!pps_init ()) {
		g_printerr ("No document backends found\n");
		return 1;
	}

	interface = pps_dbus_document_skeleton_new ();
	g_signal_connect (interface, "handle-create", G_CALLBACK (create_document), &worker);
	g_signal_connect (interface, "handle-load", G_CALLBACK (load_document), &worker);
	g_signal_connect (interface, "handle-load-fd", G_CALLBACK (load_document_fd), &worker);
	g_signal_connect (interface, "handle-get-page-info", G_CALLBACK (get_page_info), &worker);
	if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (interface),
	                                       connection, PPS_DOCUMENT_WORKER_PATH, &error)) {
		pps_shutdown ();
		goto failed;
	}
	g_signal_connect (connection, "closed", G_CALLBACK (connection_closed), loop);
	g_dbus_connection_start_message_processing (connection);
	g_main_loop_run (loop);
	g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (interface));
	g_clear_object (&worker.document);
	pps_shutdown ();
	return 0;

failed:
	g_printerr ("%s\n", error->message);
	return 1;
}

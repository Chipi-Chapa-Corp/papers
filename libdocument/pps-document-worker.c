// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <gio/gunixfdlist.h>
#include <math.h>
#include <papers-document.h>

#include "pps-document-annotation-private.h"
#include "pps-document-buffer-private.h"
#include "pps-document-factory-private.h"
#include "pps-document-info-private.h"
#include "pps-document-session-private.h"
#include "pps-document-signatures.h"
#include "pps-document-worker-private.h"

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
	guint annotation_methods = 0;
	if (PPS_IS_DOCUMENT_ANNOTATIONS (worker->document)) {
		PpsDocumentAnnotations *annotations = PPS_DOCUMENT_ANNOTATIONS (worker->document);
		if (pps_document_annotations_can_add_annotation (annotations))
			annotation_methods |= PPS_ANNOTATION_METHOD_ADD;
		if (pps_document_annotations_can_remove_annotation (annotations))
			annotation_methods |= PPS_ANNOTATION_METHOD_REMOVE;
		if (PPS_DOCUMENT_ANNOTATIONS_GET_IFACE (annotations)->over_markup)
			annotation_methods |= PPS_ANNOTATION_METHOD_OVER_MARKUP;
	}
	pps_dbus_document_complete_create (interface, invocation,
	                                   (const char *const *) interfaces->pdata, annotation_methods);
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

gboolean
pps_document_worker_check_document (Worker *worker, GType capability, GDBusMethodInvocation *invocation)
{
	if (!worker->loaded) {
		g_dbus_method_invocation_return_error_literal (
		    invocation, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
		    "The document is not loaded");
		return FALSE;
	}
	if (capability && !g_type_is_a (G_OBJECT_TYPE (worker->document), capability)) {
		g_dbus_method_invocation_return_error_literal (
		    invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		    "The backend does not support this operation");
		return FALSE;
	}
	return TRUE;
}

gboolean
pps_document_worker_check_page (Worker *worker, int page, GType capability, GDBusMethodInvocation *invocation)
{
	if (!worker->loaded || page < 0 || page >= pps_document_get_n_pages (worker->document)) {
		g_dbus_method_invocation_return_error_literal (
		    invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Page is outside the loaded document");
		return FALSE;
	}
	return pps_document_worker_check_document (worker, capability, invocation);
}

static gboolean
get_page_info (PpsDBusDocument *interface,
               GDBusMethodInvocation *invocation,
               int page,
               Worker *worker)
{
	g_autofree char *label = NULL;
	double width, height;

	if (!pps_document_worker_check_page (worker, page, G_TYPE_INVALID, invocation))
		return TRUE;
	pps_document_get_page_size (worker->document, page, &width, &height);
	label = pps_document_get_page_label (worker->document, page);
	pps_dbus_document_complete_get_page_info (interface, invocation,
	                                          width, height, label ? label : "");
	return TRUE;
}

guint
pps_document_worker_get_state (Worker *worker)
{
	guint state = pps_document_get_modified (worker->document) ? PPS_DOCUMENT_STATE_MODIFIED : 0;
	if (PPS_IS_DOCUMENT_FORMS (worker->document) &&
	    pps_document_forms_document_is_modified (PPS_DOCUMENT_FORMS (worker->document)))
		state |= PPS_DOCUMENT_STATE_FORMS_MODIFIED;
	if (PPS_IS_DOCUMENT_ANNOTATIONS (worker->document) &&
	    pps_document_annotations_document_is_modified (PPS_DOCUMENT_ANNOTATIONS (worker->document)))
		state |= PPS_DOCUMENT_STATE_ANNOTATIONS_MODIFIED;
	return state;
}

static gboolean
get_info (PpsDBusDocument *interface,
          GDBusMethodInvocation *invocation,
          Worker *worker)
{
	g_autoptr (PpsDocumentInfo) info = NULL;
	g_autoptr (GVariant) metadata = NULL;
	GVariantDict properties;
	PpsDocumentBackendInfo backend = { 0 };
	GVariantBuilder pages;

	if (!worker->loaded) {
		g_dbus_method_invocation_return_error_literal (
		    invocation, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
		    "The document is not loaded");
		return TRUE;
	}
	info = pps_document_get_info (worker->document);
	if (!info)
		info = pps_document_info_new ();
	g_variant_builder_init (&pages, G_VARIANT_TYPE ("a(dds)"));
	for (int i = 0; i < pps_document_get_n_pages (worker->document); i++) {
		double width, height;
		g_autofree char *label = pps_document_get_page_label (worker->document, i);
		pps_document_get_page_size (worker->document, i, &width, &height);
		g_variant_builder_add (&pages, "(dds)", width, height, label ? label : "");
	}
	pps_document_get_backend_info (worker->document, &backend);
	metadata = g_variant_ref_sink (_pps_document_info_to_variant (info));
	g_variant_dict_init (&properties, metadata);
	g_variant_dict_insert (&properties, "document-state", "u", pps_document_worker_get_state (worker));
	if (PPS_IS_DOCUMENT_FIND (worker->document))
		g_variant_dict_insert (&properties, "find-options", "u",
		                       pps_document_find_get_supported_options (PPS_DOCUMENT_FIND (worker->document)));
	if (PPS_IS_DOCUMENT_LINKS (worker->document))
		g_variant_dict_insert (&properties, "has-document-links", "b",
		                       pps_document_links_has_document_links (PPS_DOCUMENT_LINKS (worker->document)));
	if (PPS_IS_DOCUMENT_LAYERS (worker->document))
		g_variant_dict_insert (&properties, "has-layers", "b",
		                       pps_document_layers_has_layers (PPS_DOCUMENT_LAYERS (worker->document)));
	if (PPS_IS_DOCUMENT_ATTACHMENTS (worker->document))
		g_variant_dict_insert (&properties, "has-attachments", "b",
		                       pps_document_attachments_has_attachments (PPS_DOCUMENT_ATTACHMENTS (worker->document)));
	pps_dbus_document_complete_get_info (interface, invocation,
	                                     g_variant_dict_end (&properties),
	                                     g_variant_builder_end (&pages),
	                                     backend.name ? backend.name : "",
	                                     backend.version ? backend.version : "",
	                                     PPS_IS_DOCUMENT_SECURITY (worker->document) &&
	                                         pps_document_security_has_document_security (PPS_DOCUMENT_SECURITY (worker->document)));
	return TRUE;
}

static gboolean
save_document (PpsDBusDocument *interface,
               GDBusMethodInvocation *invocation,
               const char *uri,
               Worker *worker)
{
	g_autoptr (GError) error = NULL;

	if (!worker->loaded) {
		g_dbus_method_invocation_return_error_literal (
		    invocation, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
		    "The document is not loaded");
	} else if (!PPS_DOCUMENT_GET_CLASS (worker->document)->save) {
		g_dbus_method_invocation_return_error_literal (
		    invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		    "The backend cannot save documents");
	} else if (!pps_document_save (worker->document, uri, &error)) {
		g_dbus_method_invocation_return_gerror (invocation, error);
	} else {
		pps_dbus_document_complete_save (interface, invocation, pps_document_worker_get_state (worker));
	}
	return TRUE;
}

PpsRenderContext *
pps_document_worker_create_render_context (
    Worker *worker, int page_index, GType capability, int rotation, double scale, int target_width, int target_height, guint annotations, GDBusMethodInvocation *invocation)
{
	g_autoptr (PpsPage) page = NULL;
	PpsRenderContext *context;
	double width, height;

	if (!pps_document_worker_check_page (worker, page_index, capability, invocation))
		return NULL;
	if (rotation < 0 || rotation > 270 || rotation % 90 != 0 ||
	    !isfinite (scale) ||
	    !((target_width == -1 && target_height == -1 && scale > 0) ||
	      (target_width > 0 && target_height > 0)))
		goto invalid;
	pps_document_get_page_size (worker->document, page_index, &width, &height);
	if (rotation == 90 || rotation == 270) {
		double swap = width;
		width = height;
		height = swap;
	}
	width = target_width > 0 ? target_width : floor (width * scale + 0.5);
	height = target_height > 0 ? target_height : floor (height * scale + 0.5);
	if (!isfinite (width) || !isfinite (height) || width < 1 || height < 1 ||
	    width > G_MAXINT || height > G_MAXINT ||
	    cairo_format_stride_for_width (CAIRO_FORMAT_ARGB32, (int) width) < 0)
		goto invalid;

	page = pps_document_get_page (worker->document, page_index);
	context = pps_render_context_new (page, rotation, scale, annotations);
	pps_render_context_set_target_size (context, target_width, target_height);
	return context;

invalid:
	g_dbus_method_invocation_return_error_literal (
	    invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
	    "Invalid render dimensions or rotation");
	return NULL;
}

static gboolean
render (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, GUnixFDList *input_fds, int page_index, int rotation, double scale, int target_width, int target_height, guint annotations, gboolean thumbnail, Worker *worker)
{
	g_autoptr (PpsRenderContext) context = pps_document_worker_create_render_context (
	    worker, page_index, G_TYPE_INVALID, rotation, scale,
	    target_width, target_height, annotations, invocation);
	g_autoptr (GUnixFDList) descriptors = NULL;
	g_autoptr (GError) error = NULL;
	if (!context)
		return TRUE;
	cairo_surface_t *surface = thumbnail ? pps_document_get_thumbnail_surface (worker->document, context) : pps_document_render (worker->document, context);
	GVariant *buffer = _pps_document_surface_to_buffer (surface, &descriptors, &error);
	if (surface)
		cairo_surface_destroy (surface);
	if (!buffer)
		g_dbus_method_invocation_return_gerror (invocation, error);
	else
		pps_dbus_document_complete_render (interface, invocation, descriptors, buffer);
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
	g_autoptr (PpsDocumentObjects) objects = _pps_document_objects_new ();
	Worker worker = { .objects = objects };

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
	g_signal_connect (interface, "handle-get-info", G_CALLBACK (get_info), &worker);
	g_signal_connect (interface, "handle-save", G_CALLBACK (save_document), &worker);
	g_signal_connect (interface, "handle-render", G_CALLBACK (render), &worker);
	pps_document_worker_bind_text (&worker, interface);
	pps_document_worker_bind_selection (&worker, interface);
	pps_document_worker_bind_links (&worker, interface);
	pps_document_worker_bind_forms (&worker, interface);
	pps_document_worker_bind_attachments (&worker, interface);
	pps_document_worker_bind_annotations (&worker, interface);
	pps_document_worker_bind_fonts (&worker, interface);
	if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (interface),
	                                       connection, PPS_DOCUMENT_WORKER_PATH, &error)) {
		pps_shutdown ();
		goto failed;
	}
	g_signal_connect (connection, "closed", G_CALLBACK (connection_closed), loop);
	g_dbus_connection_start_message_processing (connection);
	g_main_loop_run (loop);
	g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (interface));
	pps_document_worker_clear_links (&worker);
	g_clear_pointer (&worker.forms, g_hash_table_unref);
	g_clear_pointer (&worker.attachments, g_ptr_array_unref);
	g_clear_pointer (&worker.annotations, g_hash_table_unref);
	g_clear_object (&worker.document);
	pps_shutdown ();
	return 0;

failed:
	g_printerr ("%s\n", error->message);
	return 1;
}

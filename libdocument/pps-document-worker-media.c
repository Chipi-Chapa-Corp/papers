// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-buffer-private.h"
#include "pps-document-mapping-private.h"
#include "pps-document-worker-private.h"
#include "pps-media-private.h"

static void
clear_mapping (gpointer data)
{
	if (data)
		pps_mapping_list_unref (data);
}

static GVariant *
encode_media (GObject *object, gpointer data)
{
	PpsMedia *media = PPS_MEDIA (object);
	gboolean embedded = _pps_media_get_temp_file (media) != NULL;
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add (&builder, "{sv}", "handle", g_variant_new_uint64 (_pps_document_objects_export (data, object)));
	g_variant_builder_add (&builder, "{sv}", "embedded", g_variant_new_boolean (embedded));
	g_variant_builder_add (&builder, "{sv}", "controls", g_variant_new_boolean (pps_media_get_show_controls (media)));
	if (!embedded)
		_pps_variant_add_string (&builder, "uri", pps_media_get_uri (media));
	return g_variant_builder_end (&builder);
}

static gboolean
get_mapping (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, int index, Worker *worker)
{
	if (!pps_document_worker_check_page (worker, index, PPS_TYPE_DOCUMENT_MEDIA, invocation))
		return TRUE;
	if (!worker->media)
		worker->media = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, clear_mapping);
	if (!g_hash_table_contains (worker->media, GINT_TO_POINTER (index))) {
		g_autoptr (PpsPage) page = pps_document_get_page (worker->document, index);
		g_hash_table_insert (worker->media, GINT_TO_POINTER (index),
		                     pps_document_media_get_media_mapping (PPS_DOCUMENT_MEDIA (worker->document), page));
	}
	PpsMappingList *mappings = g_hash_table_lookup (worker->media, GINT_TO_POINTER (index));
	pps_dbus_document_complete_get_media_mapping (interface, invocation, mappings != NULL,
	                                              _pps_mapping_list_to_variant (mappings, encode_media, worker->objects));
	return TRUE;
}

static gboolean
get_data (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, GUnixFDList *input_fds, guint64 id, Worker *worker)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_MEDIA, invocation))
		return TRUE;
	PpsMedia *media = (PpsMedia *) _pps_document_objects_lookup (worker->objects, id, PPS_TYPE_MEDIA);
	if (!media) {
		g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid document media handle");
		return TRUE;
	}
	GFile *file = _pps_media_get_temp_file (media);
	if (!file) {
		g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Media is not embedded");
		return TRUE;
	}
	g_autoptr (GError) error = NULL;
	g_autofree char *path = g_file_get_path (file);
	g_autoptr (GMappedFile) mapping = g_mapped_file_new (path, FALSE, &error);
	if (!mapping) {
		g_dbus_method_invocation_return_gerror (invocation, error);
		return TRUE;
	}
	g_autoptr (GBytes) bytes = g_mapped_file_get_bytes (mapping);
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	int handle = _pps_document_bytes_to_buffer (bytes, descriptors, &error);
	if (handle == -1)
		g_dbus_method_invocation_return_gerror (invocation, error);
	else
		pps_dbus_document_complete_get_media_data (interface, invocation, descriptors, g_bytes_get_size (bytes), g_variant_new_handle (handle));
	return TRUE;
}

void
pps_document_worker_bind_media (Worker *worker, PpsDBusDocument *interface)
{
	g_signal_connect (interface, "handle-get-media-mapping", G_CALLBACK (get_mapping), worker);
	g_signal_connect (interface, "handle-get-media-data", G_CALLBACK (get_data), worker);
}

// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-buffer-private.h"
#include "pps-document-mapping-private.h"
#include "pps-document-worker-private.h"

static GVariant *
encode_image (GObject *image, gpointer data)
{
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add (&builder, "{sv}", "handle", g_variant_new_uint64 (_pps_document_objects_export (data, image)));
	g_variant_builder_add (&builder, "{sv}", "image", g_variant_new_int32 (pps_image_get_id (PPS_IMAGE (image))));
	return g_variant_builder_end (&builder);
}

static gboolean
get_mapping (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, int index, Worker *worker)
{
	if (!pps_document_worker_check_page (worker, index, PPS_TYPE_DOCUMENT_IMAGES, invocation))
		return TRUE;
	if (!worker->images)
		worker->images = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) pps_mapping_list_unref);
	PpsMappingList *mappings = g_hash_table_lookup (worker->images, GINT_TO_POINTER (index));
	if (!mappings) {
		g_autoptr (PpsPage) page = pps_document_get_page (worker->document, index);
		mappings = pps_document_images_get_image_mapping (PPS_DOCUMENT_IMAGES (worker->document), page);
		if (mappings)
			g_hash_table_insert (worker->images, GINT_TO_POINTER (index), mappings);
	}
	pps_dbus_document_complete_get_image_mapping (interface, invocation, mappings != NULL,
	                                              _pps_mapping_list_to_variant (mappings, encode_image, worker->objects));
	return TRUE;
}

static gboolean
get_image (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, GUnixFDList *input_fds, guint64 id, Worker *worker)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_IMAGES, invocation))
		return TRUE;
	PpsImage *image = (PpsImage *) _pps_document_objects_lookup (worker->objects, id, PPS_TYPE_IMAGE);
	if (!image) {
		g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid document image handle");
		return TRUE;
	}
	g_autoptr (GdkPixbuf) pixbuf = pps_document_images_get_image (PPS_DOCUMENT_IMAGES (worker->document), image);
	g_autoptr (GUnixFDList) descriptors = NULL;
	g_autoptr (GError) error = NULL;
	GVariant *buffer = pixbuf ? _pps_document_pixbuf_to_buffer (pixbuf, &descriptors, &error) : NULL;
	if (pixbuf && !buffer)
		g_dbus_method_invocation_return_gerror (invocation, error);
	else
		pps_dbus_document_complete_get_image (interface, invocation, descriptors, pixbuf != NULL,
		                                      buffer ? buffer : g_variant_new ("(hiiib)", -1, 0, 0, 0, FALSE));
	return TRUE;
}

void
pps_document_worker_bind_images (Worker *worker, PpsDBusDocument *interface)
{
	g_signal_connect (interface, "handle-get-image-mapping", G_CALLBACK (get_mapping), worker);
	g_signal_connect (interface, "handle-get-image", G_CALLBACK (get_image), worker);
}

// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-buffer-private.h"
#include "pps-document-mapping-private.h"
#include "pps-document-proxy-private.h"

typedef struct {
	PpsDocumentObjects *objects;
	int page;
} ImageMapping;

static PpsDBusDocument *
get_proxy (PpsDocumentImages *document)
{
	return _pps_document_proxy_get_session (PPS_DOCUMENT (document))->proxy;
}

static GObject *
decode_image (GVariant *value, gpointer data, GError **error)
{
	ImageMapping *mapping = data;
	g_autoptr (PpsImage) image = NULL;
	guint64 handle;
	int id;
	if (!g_variant_lookup (value, "handle", "t", &handle) || !g_variant_lookup (value, "image", "i", &id) || id < 0)
		goto invalid;
	image = pps_image_new (mapping->page, id);
	if (!_pps_document_objects_bind (mapping->objects, handle, G_OBJECT (image)))
		goto invalid;
	return G_OBJECT (g_steal_pointer (&image));
invalid:
	g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid document image mapping");
	return NULL;
}

static PpsMappingList *
get_mapping (PpsDocumentImages *document, PpsPage *page)
{
	g_autoptr (PpsDocumentImages) hold = g_object_ref (document);
	g_autoptr (GVariant) mappings = NULL;
	g_autoptr (GError) error = NULL;
	gboolean present;
	if (!pps_dbus_document_call_get_image_mapping_sync (get_proxy (document), page->index,
	                                                    G_DBUS_CALL_FLAGS_NONE, -1, &present, &mappings, NULL, &error) ||
	    !present)
		return NULL;
	ImageMapping mapping = { _pps_document_proxy_get_objects (PPS_DOCUMENT (document)), page->index };
	return _pps_mapping_list_from_variant (mappings, page->index, decode_image, &mapping, &error);
}

static GdkPixbuf *
get_image (PpsDocumentImages *document, PpsImage *image)
{
	g_autoptr (PpsDocumentImages) hold = g_object_ref (document);
	guint64 id = _pps_document_objects_get_id (_pps_document_proxy_get_objects (PPS_DOCUMENT (document)), G_OBJECT (image));
	g_autoptr (GVariant) buffer = NULL;
	g_autoptr (GUnixFDList) descriptors = NULL;
	g_autoptr (GError) error = NULL;
	gboolean present;
	if (!pps_dbus_document_call_get_image_sync (get_proxy (document), id, G_DBUS_CALL_FLAGS_NONE, -1,
	                                            NULL, &present, &buffer, &descriptors, NULL, &error) ||
	    !present)
		return NULL;
	return _pps_document_pixbuf_from_buffer (buffer, descriptors, &error);
}

void
_pps_document_proxy_images_iface_init (PpsDocumentImagesInterface *iface)
{
	iface->get_image_mapping = get_mapping;
	iface->get_image = get_image;
}

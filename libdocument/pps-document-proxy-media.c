// SPDX-License-Identifier: GPL-2.0-or-later

#include <gio/gunixoutputstream.h>
#include <glib/gstdio.h>

#include "pps-document-buffer-private.h"
#include "pps-document-mapping-private.h"
#include "pps-document-proxy-private.h"
#include "pps-file-helpers.h"
#include "pps-media-private.h"

typedef struct {
	PpsDocument *document;
	PpsPage *page;
} MediaMapping;

static GFile *
download_file (PpsDocument *document, guint64 id, GError **error)
{
	g_autoptr (GUnixFDList) descriptors = NULL;
	g_autoptr (GVariant) buffer = NULL;
	guint64 size;
	if (!pps_dbus_document_call_get_media_data_sync (_pps_document_proxy_get_session (document)->proxy, id,
	                                                 G_DBUS_CALL_FLAGS_NONE, -1, NULL, &size, &buffer, &descriptors, NULL, error))
		return NULL;
	g_autoptr (GBytes) bytes = _pps_document_bytes_from_buffer (g_variant_get_handle (buffer), size, descriptors, error);
	if (!bytes)
		return NULL;
	g_autofree char *path = NULL;
	int fd = pps_mkstemp ("media.XXXXXX", &path, error);
	if (fd == -1)
		return NULL;
	g_autoptr (GOutputStream) stream = g_unix_output_stream_new (fd, TRUE);
	if ((size && !g_output_stream_write_all (stream, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes), NULL, NULL, error)) ||
	    !g_output_stream_close (stream, NULL, error)) {
		g_unlink (path);
		return NULL;
	}
	return g_file_new_for_path (path);
}

static GObject *
decode_media (GVariant *value, gpointer data, GError **error)
{
	MediaMapping *mapping = data;
	PpsDocumentObjects *objects = _pps_document_proxy_get_objects (mapping->document);
	g_autoptr (GFile) file = NULL;
	g_autoptr (PpsMedia) media = NULL;
	g_autofree char *uri = NULL;
	guint64 id;
	gboolean embedded, controls, cached = FALSE;
	if (!g_variant_lookup (value, "handle", "t", &id) || !id ||
	    !g_variant_lookup (value, "embedded", "b", &embedded) || !g_variant_lookup (value, "controls", "b", &controls))
		goto invalid;
	if (embedded) {
		GObject *asset = _pps_document_objects_lookup (objects, id, G_TYPE_FILE);
		cached = asset != NULL;
		file = cached ? g_object_ref (G_FILE (asset)) : download_file (mapping->document, id, error);
		if (!file)
			return NULL;
		uri = g_file_get_uri (file);
	} else if (!_pps_variant_read_string (value, "uri", &uri) || !uri) {
		goto invalid;
	}
	media = pps_media_new_for_uri (mapping->page, uri);
	pps_media_set_show_controls (media, controls);
	if (file) {
		_pps_media_set_temp_file (media, file);
		if (!cached && !_pps_document_objects_import (objects, id, G_OBJECT (file)))
			goto invalid;
	}
	return G_OBJECT (g_steal_pointer (&media));
invalid:
	g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid document media mapping");
	return NULL;
}

static PpsMappingList *
get_mapping (PpsDocumentMedia *document, PpsPage *page)
{
	g_autoptr (GVariant) mappings = NULL;
	g_autoptr (GError) error = NULL;
	gboolean present;
	if (!pps_dbus_document_call_get_media_mapping_sync (_pps_document_proxy_get_session (PPS_DOCUMENT (document))->proxy,
	                                                    page->index, G_DBUS_CALL_FLAGS_NONE, -1, &present, &mappings, NULL, &error) ||
	    !present)
		return NULL;
	MediaMapping mapping = { PPS_DOCUMENT (document), page };
	return _pps_mapping_list_from_variant (mappings, page->index, decode_media, &mapping, &error);
}

void
_pps_document_proxy_media_iface_init (PpsDocumentMediaInterface *iface)
{
	iface->get_media_mapping = get_mapping;
}

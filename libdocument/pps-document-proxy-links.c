// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-link-private.h"
#include "pps-document-mapping-private.h"
#include "pps-document-proxy-private.h"

static PpsDBusDocument *
get_proxy (gpointer document)
{
	return _pps_document_proxy_get_session (PPS_DOCUMENT (document))->proxy;
}

static gboolean
has_feature (gpointer document, const char *key)
{
	GVariant *metadata = _pps_document_proxy_get_metadata (PPS_DOCUMENT (document));
	gboolean value = FALSE;
	if (metadata)
		g_variant_lookup (metadata, key, "b", &value);
	return value;
}

static gboolean
has_document_links (PpsDocumentLinks *document)
{
	return has_feature (document, "has-document-links");
}

static GListModel *
get_links_model (PpsDocumentLinks *document)
{
	g_autoptr (GVariant) rows = NULL;
	g_autoptr (GError) error = NULL;
	gboolean present;
	if (!pps_dbus_document_call_get_outlines_sync (get_proxy (document), G_DBUS_CALL_FLAGS_NONE, -1,
	                                               &present, &rows, NULL, &error) ||
	    !present)
		return NULL;
	return _pps_outlines_from_variant (rows, _pps_document_proxy_get_objects (PPS_DOCUMENT (document)), &error);
}

static GObject *
decode_link (GVariant *value, gpointer data, GError **error)
{
	return G_OBJECT (_pps_link_from_variant (value, data, error));
}

static PpsMappingList *
get_links (PpsDocumentLinks *document, PpsPage *page)
{
	g_autoptr (GVariant) mappings = NULL;
	g_autoptr (GError) error = NULL;
	gboolean present;
	if (!pps_dbus_document_call_get_links_sync (get_proxy (document), page->index,
	                                            G_DBUS_CALL_FLAGS_NONE, -1, &present, &mappings, NULL, &error) ||
	    !present)
		return NULL;
	return _pps_mapping_list_from_variant (mappings, page->index, decode_link,
	                                       _pps_document_proxy_get_objects (PPS_DOCUMENT (document)), &error);
}

static PpsLinkDest *
find_link_dest (PpsDocumentLinks *document, const char *name)
{
	g_autoptr (GVariant) destination = NULL;
	g_autoptr (GError) error = NULL;
	if (!pps_dbus_document_call_find_link_dest_sync (get_proxy (document), name,
	                                                 G_DBUS_CALL_FLAGS_NONE, -1, &destination, NULL, &error))
		return NULL;
	return _pps_link_dest_from_variant (destination, &error);
}

static int
find_link_page (PpsDocumentLinks *document, const char *name)
{
	int page = -1;
	pps_dbus_document_call_find_link_page_sync (get_proxy (document), name,
	                                            G_DBUS_CALL_FLAGS_NONE, -1, &page, NULL, NULL);
	return page;
}

static gboolean
has_layers (PpsDocumentLayers *document)
{
	return has_feature (document, "has-layers");
}

static GListModel *
get_layers (PpsDocumentLayers *document)
{
	GListModel *cached = g_object_get_data (G_OBJECT (document), "pps-proxy-layers");
	g_autoptr (GVariant) rows = NULL;
	g_autoptr (GError) error = NULL;
	gboolean present;
	if (cached)
		return g_object_ref (cached);
	if (!pps_dbus_document_call_get_layers_sync (get_proxy (document), G_DBUS_CALL_FLAGS_NONE, -1,
	                                             &present, &rows, NULL, &error) ||
	    !present)
		return NULL;
	GListModel *model = _pps_layers_from_variant (rows, _pps_document_proxy_get_objects (PPS_DOCUMENT (document)), &error);
	if (model)
		g_object_set_data_full (G_OBJECT (document), "pps-proxy-layers", g_object_ref (model), g_object_unref);
	return model;
}

static void
set_layer_visibility (PpsDocumentLayers *document, PpsLayer *layer, gboolean visible)
{
	guint64 id = _pps_document_objects_get_id (_pps_document_proxy_get_objects (PPS_DOCUMENT (document)), G_OBJECT (layer));
	pps_dbus_document_call_set_layer_visibility_sync (get_proxy (document), id, visible,
	                                                  G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
}

static void
show_layer (PpsDocumentLayers *document, PpsLayer *layer)
{
	set_layer_visibility (document, layer, TRUE);
}

static void
hide_layer (PpsDocumentLayers *document, PpsLayer *layer)
{
	set_layer_visibility (document, layer, FALSE);
}

static gboolean
layer_is_visible (PpsDocumentLayers *document, PpsLayer *layer)
{
	gboolean visible = FALSE;
	guint64 id = _pps_document_objects_get_id (_pps_document_proxy_get_objects (PPS_DOCUMENT (document)), G_OBJECT (layer));
	pps_dbus_document_call_get_layer_visibility_sync (get_proxy (document), id,
	                                                  G_DBUS_CALL_FLAGS_NONE, -1, &visible, NULL, NULL);
	return visible;
}

void
_pps_document_proxy_links_iface_init (PpsDocumentLinksInterface *iface)
{
	iface->has_document_links = has_document_links;
	iface->get_links_model = get_links_model;
	iface->get_links = get_links;
	iface->find_link_dest = find_link_dest;
	iface->find_link_page = find_link_page;
}

void
_pps_document_proxy_layers_iface_init (PpsDocumentLayersInterface *iface)
{
	iface->has_layers = has_layers;
	iface->get_layers = get_layers;
	iface->show_layer = show_layer;
	iface->hide_layer = hide_layer;
	iface->layer_is_visible = layer_is_visible;
}

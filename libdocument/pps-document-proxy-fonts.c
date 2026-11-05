// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-proxy-private.h"
#include "pps-document-variant-private.h"
#include "pps-font-description.h"

static PpsDBusDocument *
get_proxy (PpsDocumentFonts *document)
{
	return _pps_document_proxy_get_session (PPS_DOCUMENT (document))->proxy;
}

static void
scan (PpsDocumentFonts *document)
{
	g_autoptr (PpsDocumentFonts) hold = g_object_ref (document);
	pps_dbus_document_call_scan_fonts_sync (get_proxy (document), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
}

static GListModel *
get_model (PpsDocumentFonts *document)
{
	g_autoptr (PpsDocumentFonts) hold = g_object_ref (document);
	g_autoptr (GVariant) fonts = NULL;
	g_autoptr (GError) error = NULL;
	gboolean present;
	if (!pps_dbus_document_call_get_fonts_sync (get_proxy (document), G_DBUS_CALL_FLAGS_NONE, -1, &present, &fonts, NULL, &error) || !present)
		return NULL;
	g_autoptr (GListStore) model = g_list_store_new (PPS_TYPE_FONT_DESCRIPTION);
	for (gsize i = 0; i < g_variant_n_children (fonts); i++) {
		g_autoptr (GVariant) value = g_variant_get_child_value (fonts, i);
		g_autofree char *name = NULL, *details = NULL;
		if (!_pps_variant_read_string (value, "name", &name) || !_pps_variant_read_string (value, "details", &details))
			return NULL;
		g_autoptr (PpsFontDescription) font = g_object_new (PPS_TYPE_FONT_DESCRIPTION, "name", name, "details", details, NULL);
		g_list_store_append (model, font);
	}
	return G_LIST_MODEL (g_steal_pointer (&model));
}

static const char *
get_summary (PpsDocumentFonts *document)
{
	g_autoptr (PpsDocumentFonts) hold = g_object_ref (document);
	g_autofree char *summary = NULL;
	gboolean present;
	if (!pps_dbus_document_call_get_fonts_summary_sync (get_proxy (document), G_DBUS_CALL_FLAGS_NONE, -1, &present, &summary, NULL, NULL) || !present)
		return NULL;
	g_object_set_data_full (G_OBJECT (document), "pps-proxy-fonts-summary", g_steal_pointer (&summary), g_free);
	return g_object_get_data (G_OBJECT (document), "pps-proxy-fonts-summary");
}

void
_pps_document_proxy_fonts_iface_init (PpsDocumentFontsInterface *iface)
{
	iface->scan = scan;
	iface->get_model = get_model;
	iface->get_fonts_summary = get_summary;
}

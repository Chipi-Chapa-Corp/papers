// SPDX-License-Identifier: GPL-2.0-or-later

#include <math.h>

#include "pps-document-geometry-private.h"
#include "pps-document-proxy-private.h"

static PpsDBusDocument *
get_proxy (gpointer document)
{
	return _pps_document_proxy_get_session (PPS_DOCUMENT (document))->proxy;
}

static char *
get_text (PpsDocumentText *document, PpsPage *page)
{
	g_autoptr (PpsDocumentText) hold = g_object_ref (document);
	g_autofree char *text = NULL;
	gboolean present;
	if (!pps_dbus_document_call_get_text_sync (get_proxy (document), page->index,
	                                           G_DBUS_CALL_FLAGS_NONE, -1, &present, &text, NULL, NULL))
		return NULL;
	return present ? g_steal_pointer (&text) : NULL;
}

static gboolean
get_text_layout (PpsDocumentText *document, PpsPage *page, PpsRectangle **areas, guint *n_areas)
{
	g_autoptr (PpsDocumentText) hold = g_object_ref (document);
	g_autoptr (GVariant) rectangles = NULL;
	g_autoptr (GError) error = NULL;
	g_autofree PpsRectangle *result = NULL;
	gboolean present;
	guint count;
	if (!pps_dbus_document_call_get_text_layout_sync (
		get_proxy (document), page->index, G_DBUS_CALL_FLAGS_NONE, -1,
		&present, &rectangles, NULL, NULL) ||
	    !present ||
	    !_pps_rectangles_from_variant (rectangles, &result, &count, &error))
		return FALSE;
	if (areas)
		*areas = g_steal_pointer (&result);
	if (n_areas)
		*n_areas = count;
	return TRUE;
}

static cairo_region_t *
get_text_mapping (PpsDocumentText *document, PpsPage *page)
{
	g_autoptr (PpsDocumentText) hold = g_object_ref (document);
	g_autoptr (GVariant) region = NULL;
	g_autoptr (GError) error = NULL;
	gboolean present;
	if (!pps_dbus_document_call_get_text_mapping_sync (
		get_proxy (document), page->index, G_DBUS_CALL_FLAGS_NONE, -1,
		&present, &region, NULL, NULL) ||
	    !present)
		return NULL;
	return _pps_region_from_variant (region, &error);
}

static PangoAttrList *
get_text_attrs (PpsDocumentText *document, PpsPage *page)
{
	g_autoptr (PpsDocumentText) hold = g_object_ref (document);
	g_autofree char *attributes = NULL;
	gboolean present;
	if (!pps_dbus_document_call_get_text_attrs_sync (
		get_proxy (document), page->index, G_DBUS_CALL_FLAGS_NONE, -1,
		&present, &attributes, NULL, NULL) ||
	    !present)
		return NULL;
	return pango_attr_list_from_string (attributes);
}

static char *
get_text_in_area (PpsDocumentText *document, PpsPage *page, PpsRectangle *area)
{
	g_autoptr (PpsDocumentText) hold = g_object_ref (document);
	g_autofree char *text = NULL;
	gboolean present;
	if (!pps_dbus_document_call_get_text_in_area_sync (
		get_proxy (document), page->index,
		g_variant_new ("(dddd)", area->x1, area->y1, area->x2, area->y2),
		G_DBUS_CALL_FLAGS_NONE, -1, &present, &text, NULL, NULL))
		return NULL;
	return present ? g_steal_pointer (&text) : NULL;
}

static GList *
find_text (PpsDocumentFind *document, PpsPage *page, const char *text, PpsFindOptions options)
{
	g_autoptr (PpsDocumentFind) hold = g_object_ref (document);
	g_autoptr (GVariant) matches = NULL;
	GList *result = NULL;
	if (!pps_dbus_document_call_find_text_sync (
		get_proxy (document), page->index, text, options,
		G_DBUS_CALL_FLAGS_NONE, -1, &matches, NULL, NULL))
		return NULL;
	for (gsize i = 0; i < g_variant_n_children (matches); i++) {
		PpsFindRectangle *rectangle = pps_find_rectangle_new ();
		g_variant_get_child (matches, i, "(ddddbb)", &rectangle->x1, &rectangle->y1,
		                     &rectangle->x2, &rectangle->y2, &rectangle->next_line, &rectangle->after_hyphen);
		if (!isfinite (rectangle->x1) || !isfinite (rectangle->y1) ||
		    !isfinite (rectangle->x2) || !isfinite (rectangle->y2)) {
			pps_find_rectangle_free (rectangle);
			g_list_free_full (result, (GDestroyNotify) pps_find_rectangle_free);
			return NULL;
		}
		result = g_list_prepend (result, rectangle);
	}
	return g_list_reverse (result);
}

static PpsFindOptions
get_supported_options (PpsDocumentFind *document)
{
	GVariant *metadata = _pps_document_proxy_get_metadata (PPS_DOCUMENT (document));
	guint options = 0;
	if (metadata)
		g_variant_lookup (metadata, "find-options", "u", &options);
	return options;
}

void
_pps_document_proxy_text_iface_init (PpsDocumentTextInterface *iface)
{
	iface->get_text = get_text;
	iface->get_text_layout = get_text_layout;
	iface->get_text_mapping = get_text_mapping;
	iface->get_text_attrs = get_text_attrs;
	iface->get_text_in_area = get_text_in_area;
}

void
_pps_document_proxy_find_iface_init (PpsDocumentFindInterface *iface)
{
	iface->get_supported_options = get_supported_options;
	iface->find_text = find_text;
}

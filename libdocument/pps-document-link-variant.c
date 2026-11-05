// SPDX-License-Identifier: GPL-2.0-or-later

#include <math.h>

#include "pps-document-link-private.h"
#include "pps-document-variant-private.h"

static void
invalid_object (GError **error)
{
	if (!error || !*error)
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid document link object");
}

GVariant *
_pps_link_dest_to_variant (PpsLinkDest *dest)
{
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
	if (dest) {
		double left, top, bottom, right, zoom;
		guint change;
		g_object_get (dest, "left", &left, "top", &top, "bottom", &bottom,
		              "right", &right, "zoom", &zoom, "change", &change, NULL);
		g_variant_builder_add (&builder, "{sv}", "type", g_variant_new_int32 (pps_link_dest_get_dest_type (dest)));
		g_variant_builder_add (&builder, "{sv}", "page", g_variant_new_int32 (pps_link_dest_get_page (dest)));
		g_variant_builder_add (&builder, "{sv}", "coordinates", g_variant_new ("(ddddd)", left, top, bottom, right, zoom));
		g_variant_builder_add (&builder, "{sv}", "change", g_variant_new_uint32 (change));
		_pps_variant_add_string (&builder, "named", pps_link_dest_get_named_dest (dest));
		_pps_variant_add_string (&builder, "page-label", pps_link_dest_get_page_label (dest));
	}
	return g_variant_builder_end (&builder);
}

PpsLinkDest *
_pps_link_dest_from_variant (GVariant *value, GError **error)
{
	int type, page;
	double left, top, bottom, right, zoom;
	guint change;
	g_autofree char *named = NULL;
	g_autofree char *label = NULL;
	if (!g_variant_is_of_type (value, G_VARIANT_TYPE_VARDICT))
		goto invalid;
	if (!g_variant_n_children (value))
		return NULL;
	if (!g_variant_lookup (value, "type", "i", &type) || type < 0 || type > PPS_LINK_DEST_TYPE_UNKNOWN ||
	    !g_variant_lookup (value, "page", "i", &page) || page < -1 ||
	    !g_variant_lookup (value, "coordinates", "(ddddd)", &left, &top, &bottom, &right, &zoom) ||
	    !g_variant_lookup (value, "change", "u", &change) ||
	    !isfinite (left) || !isfinite (top) || !isfinite (bottom) || !isfinite (right) || !isfinite (zoom) || zoom < 0 ||
	    !_pps_variant_read_string (value, "named", &named) ||
	    !_pps_variant_read_string (value, "page-label", &label))
		goto invalid;
	return g_object_new (PPS_TYPE_LINK_DEST, "type", type, "page", page,
	                     "left", left, "top", top, "bottom", bottom, "right", right, "zoom", zoom,
	                     "change", change, "named", named, "page-label", label, NULL);
invalid:
	invalid_object (error);
	return NULL;
}

GVariant *
_pps_layer_to_variant (PpsLayer *layer, PpsDocumentObjects *objects)
{
	GVariantBuilder builder;
	g_autofree char *title = NULL;
	gboolean enabled, title_only;
	g_object_get (layer, "title", &title, "enabled", &enabled, "title-only", &title_only, NULL);
	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add (&builder, "{sv}", "id", g_variant_new_uint64 (_pps_document_objects_export (objects, G_OBJECT (layer))));
	g_variant_builder_add (&builder, "{sv}", "rb-group", g_variant_new_int32 (pps_layer_get_rb_group (layer)));
	g_variant_builder_add (&builder, "{sv}", "enabled", g_variant_new_boolean (enabled));
	g_variant_builder_add (&builder, "{sv}", "title-only", g_variant_new_boolean (title_only));
	_pps_variant_add_string (&builder, "title", title);
	return g_variant_builder_end (&builder);
}

PpsLayer *
_pps_layer_from_variant (GVariant *value, PpsDocumentObjects *objects, GError **error)
{
	guint64 id;
	int rb_group;
	gboolean enabled, title_only;
	g_autofree char *title = NULL;
	g_autoptr (PpsLayer) layer = NULL;
	if (!g_variant_is_of_type (value, G_VARIANT_TYPE_VARDICT) ||
	    !g_variant_lookup (value, "id", "t", &id) || !id ||
	    !g_variant_lookup (value, "rb-group", "i", &rb_group) ||
	    !g_variant_lookup (value, "enabled", "b", &enabled) ||
	    !g_variant_lookup (value, "title-only", "b", &title_only) ||
	    !_pps_variant_read_string (value, "title", &title))
		goto invalid;
	GObject *existing = _pps_document_objects_lookup (objects, id, PPS_TYPE_LAYER);
	if (existing)
		return g_object_ref (PPS_LAYER (existing));
	layer = pps_layer_new (rb_group);
	g_object_set (layer, "title", title, "enabled", enabled, "title-only", title_only, NULL);
	if (!_pps_document_objects_import (objects, id, G_OBJECT (layer))) {
		existing = _pps_document_objects_lookup (objects, id, PPS_TYPE_LAYER);
		if (existing)
			return g_object_ref (PPS_LAYER (existing));
		goto invalid;
	}
	return g_steal_pointer (&layer);
invalid:
	invalid_object (error);
	return NULL;
}

static GVariant *
layers_to_variant (GList *layers, PpsDocumentObjects *objects)
{
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));
	for (GList *item = layers; item; item = item->next)
		g_variant_builder_add_value (&builder, _pps_layer_to_variant (item->data, objects));
	return g_variant_builder_end (&builder);
}

static gboolean
read_layers (GVariant *dictionary, const char *name, PpsDocumentObjects *objects, GList **result, GError **error)
{
	g_autoptr (GVariant) value = g_variant_lookup_value (dictionary, name, G_VARIANT_TYPE ("aa{sv}"));
	if (!value)
		return FALSE;
	for (gsize i = 0; i < g_variant_n_children (value); i++) {
		g_autoptr (GVariant) child = g_variant_get_child_value (value, i);
		PpsLayer *layer = _pps_layer_from_variant (child, objects, error);
		if (!layer)
			return FALSE;
		*result = g_list_prepend (*result, layer);
	}
	*result = g_list_reverse (*result);
	return TRUE;
}

static GVariant *
action_to_variant (PpsLinkAction *action, PpsDocumentObjects *objects)
{
	GVariantBuilder builder, fields;
	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
	if (!action)
		return g_variant_builder_end (&builder);
	g_variant_builder_add (&builder, "{sv}", "type", g_variant_new_int32 (pps_link_action_get_action_type (action)));
	g_variant_builder_add (&builder, "{sv}", "dest", _pps_link_dest_to_variant (pps_link_action_get_dest (action)));
	_pps_variant_add_string (&builder, "uri", pps_link_action_get_uri (action));
	_pps_variant_add_string (&builder, "filename", pps_link_action_get_filename (action));
	_pps_variant_add_string (&builder, "params", pps_link_action_get_params (action));
	_pps_variant_add_string (&builder, "name", pps_link_action_get_name (action));
	g_variant_builder_add (&builder, "{sv}", "show", layers_to_variant (pps_link_action_get_show_list (action), objects));
	g_variant_builder_add (&builder, "{sv}", "hide", layers_to_variant (pps_link_action_get_hide_list (action), objects));
	g_variant_builder_add (&builder, "{sv}", "toggle", layers_to_variant (pps_link_action_get_toggle_list (action), objects));
	g_variant_builder_init (&fields, G_VARIANT_TYPE_STRING_ARRAY);
	for (GList *item = pps_link_action_get_reset_fields (action); item; item = item->next)
		g_variant_builder_add (&fields, "s", (const char *) item->data);
	g_variant_builder_add (&builder, "{sv}", "fields", g_variant_builder_end (&fields));
	g_variant_builder_add (&builder, "{sv}", "exclude-fields", g_variant_new_boolean (pps_link_action_get_exclude_reset_fields (action)));
	return g_variant_builder_end (&builder);
}

static PpsLinkAction *
action_from_variant (GVariant *value, PpsDocumentObjects *objects, GError **error)
{
	int type;
	gboolean exclude;
	g_autofree char *uri = NULL, *filename = NULL, *params = NULL, *name = NULL;
	g_auto (GStrv) fields = NULL;
	g_autoptr (GVariant) dest_value = NULL;
	g_autoptr (PpsLinkDest) dest = NULL;
	GList *show = NULL, *hide = NULL, *toggle = NULL, *reset = NULL;
	if (!g_variant_is_of_type (value, G_VARIANT_TYPE_VARDICT))
		goto invalid;
	if (!g_variant_n_children (value))
		return NULL;
	if (!g_variant_lookup (value, "type", "i", &type) || type < 0 || type > PPS_LINK_ACTION_TYPE_RESET_FORM ||
	    !_pps_variant_read_string (value, "uri", &uri) ||
	    !_pps_variant_read_string (value, "filename", &filename) ||
	    !_pps_variant_read_string (value, "params", &params) ||
	    !_pps_variant_read_string (value, "name", &name) ||
	    !g_variant_lookup (value, "fields", "^as", &fields) ||
	    !g_variant_lookup (value, "exclude-fields", "b", &exclude))
		goto invalid;
	dest_value = g_variant_lookup_value (value, "dest", G_VARIANT_TYPE_VARDICT);
	if (!dest_value)
		goto invalid;
	dest = _pps_link_dest_from_variant (dest_value, error);
	if ((!dest && g_variant_n_children (dest_value)) ||
	    !read_layers (value, "show", objects, &show, error) ||
	    !read_layers (value, "hide", objects, &hide, error) ||
	    !read_layers (value, "toggle", objects, &toggle, error))
		goto invalid;
	if ((type == PPS_LINK_ACTION_TYPE_GOTO_DEST && !dest) ||
	    (type == PPS_LINK_ACTION_TYPE_GOTO_REMOTE && !filename) ||
	    (type == PPS_LINK_ACTION_TYPE_EXTERNAL_URI && !uri) ||
	    (type == PPS_LINK_ACTION_TYPE_LAUNCH && !filename) ||
	    (type == PPS_LINK_ACTION_TYPE_NAMED && !name))
		goto invalid;
	for (guint i = 0; fields[i]; i++)
		reset = g_list_prepend (reset, g_strdup (fields[i]));
	return g_object_new (PPS_TYPE_LINK_ACTION, "type", type, "dest", dest,
	                     "uri", uri, "filename", filename, "params", params, "name", name,
	                     "show-list", show, "hide-list", hide, "toggle-list", toggle,
	                     "reset-fields", g_list_reverse (reset), "exclude-reset-fields", exclude, NULL);
invalid:
	g_list_free_full (show, g_object_unref);
	g_list_free_full (hide, g_object_unref);
	g_list_free_full (toggle, g_object_unref);
	g_list_free_full (reset, g_free);
	invalid_object (error);
	return NULL;
}

GVariant *
_pps_link_to_variant (PpsLink *link, PpsDocumentObjects *objects)
{
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
	if (link) {
		_pps_variant_add_string (&builder, "title", pps_link_get_title (link));
		g_variant_builder_add (&builder, "{sv}", "action", action_to_variant (pps_link_get_action (link), objects));
	}
	return g_variant_builder_end (&builder);
}

PpsLink *
_pps_link_from_variant (GVariant *value, PpsDocumentObjects *objects, GError **error)
{
	g_autofree char *title = NULL;
	g_autoptr (GVariant) action_value = NULL;
	g_autoptr (PpsLinkAction) action = NULL;
	if (!g_variant_is_of_type (value, G_VARIANT_TYPE_VARDICT))
		goto invalid;
	if (!g_variant_n_children (value))
		return NULL;
	if (!_pps_variant_read_string (value, "title", &title))
		goto invalid;
	action_value = g_variant_lookup_value (value, "action", G_VARIANT_TYPE_VARDICT);
	if (!action_value)
		goto invalid;
	action = action_from_variant (action_value, objects, error);
	if (!action && g_variant_n_children (action_value))
		goto invalid;
	return pps_link_new (title, action);
invalid:
	invalid_object (error);
	return NULL;
}

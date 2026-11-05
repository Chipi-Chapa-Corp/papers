// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-geometry-private.h"
#include "pps-document-mapping-private.h"

GVariant *
_pps_mapping_list_to_variant (PpsMappingList *mappings, PpsDocumentEncodeItem encode, gpointer data)
{
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a((dddd)a{sv})"));
	for (GList *item = mappings ? pps_mapping_list_get_list (mappings) : NULL; item; item = item->next) {
		PpsMapping *mapping = item->data;
		g_variant_builder_add (&builder, "((dddd)@a{sv})", mapping->area.x1, mapping->area.y1,
		                       mapping->area.x2, mapping->area.y2, encode (mapping->data, data));
	}
	return g_variant_builder_end (&builder);
}

PpsMappingList *
_pps_mapping_list_from_variant (GVariant *value, guint page, PpsDocumentDecodeItem decode, gpointer data, GError **error)
{
	GList *mappings = NULL;
	if (!g_variant_is_of_type (value, G_VARIANT_TYPE ("a((dddd)a{sv})")))
		goto invalid;
	for (gsize i = 0; i < g_variant_n_children (value); i++) {
		g_autoptr (GVariant) area = NULL, properties = NULL;
		g_autofree PpsMapping *mapping = g_new0 (PpsMapping, 1);
		g_variant_get_child (value, i, "(@(dddd)@a{sv})", &area, &properties);
		if (!_pps_rectangle_from_variant (area, &mapping->area))
			goto invalid;
		mapping->data = decode (properties, data, error);
		if (!mapping->data)
			goto invalid;
		mappings = g_list_prepend (mappings, g_steal_pointer (&mapping));
	}
	return pps_mapping_list_new (page, g_list_reverse (mappings), g_object_unref);
invalid:
	for (GList *item = mappings; item; item = item->next) {
		PpsMapping *mapping = item->data;
		g_object_unref (mapping->data);
		g_free (mapping);
	}
	g_list_free (mappings);
	if (!error || !*error)
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid document mappings");
	return NULL;
}

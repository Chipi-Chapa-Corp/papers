// SPDX-License-Identifier: GPL-2.0-or-later

#include <math.h>

#include "pps-document-geometry-private.h"

gboolean
_pps_rectangle_from_variant (GVariant *value, PpsRectangle *area)
{
	if (!g_variant_is_of_type (value, G_VARIANT_TYPE ("(dddd)")))
		return FALSE;
	g_variant_get (value, "(dddd)", &area->x1, &area->y1, &area->x2, &area->y2);
	return isfinite (area->x1) && isfinite (area->y1) &&
	       isfinite (area->x2) && isfinite (area->y2);
}

GVariant *
_pps_rectangles_to_variant (const PpsRectangle *areas, guint n_areas)
{
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(dddd)"));
	for (guint i = 0; i < n_areas; i++)
		g_variant_builder_add (&builder, "(dddd)", areas[i].x1, areas[i].y1, areas[i].x2, areas[i].y2);
	return g_variant_builder_end (&builder);
}

gboolean
_pps_rectangles_from_variant (GVariant *value, PpsRectangle **areas, guint *n_areas, GError **error)
{
	g_autofree PpsRectangle *result = NULL;
	gsize count;
	if (!g_variant_is_of_type (value, G_VARIANT_TYPE ("a(dddd)")))
		goto invalid;
	count = g_variant_n_children (value);
	if (count > G_MAXUINT || count > G_MAXSIZE / sizeof (PpsRectangle))
		goto invalid;
	result = g_new0 (PpsRectangle, count);
	for (gsize i = 0; i < count; i++) {
		g_autoptr (GVariant) rectangle = g_variant_get_child_value (value, i);
		if (!_pps_rectangle_from_variant (rectangle, &result[i]))
			goto invalid;
	}
	*n_areas = count;
	*areas = g_steal_pointer (&result);
	return TRUE;

invalid:
	g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid document rectangles");
	return FALSE;
}

GVariant *
_pps_region_to_variant (cairo_region_t *region)
{
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(iiii)"));
	for (int i = 0; region && i < cairo_region_num_rectangles (region); i++) {
		cairo_rectangle_int_t rectangle;
		cairo_region_get_rectangle (region, i, &rectangle);
		g_variant_builder_add (&builder, "(iiii)", rectangle.x, rectangle.y,
		                       rectangle.width, rectangle.height);
	}
	return g_variant_builder_end (&builder);
}

cairo_region_t *
_pps_region_from_variant (GVariant *value, GError **error)
{
	cairo_region_t *region = cairo_region_create ();
	if (!g_variant_is_of_type (value, G_VARIANT_TYPE ("a(iiii)")))
		goto invalid;
	for (gsize i = 0; i < g_variant_n_children (value); i++) {
		cairo_rectangle_int_t rectangle;
		g_variant_get_child (value, i, "(iiii)", &rectangle.x, &rectangle.y,
		                     &rectangle.width, &rectangle.height);
		if (rectangle.width < 0 || rectangle.height < 0 ||
		    (gint64) rectangle.x + rectangle.width > G_MAXINT ||
		    (gint64) rectangle.y + rectangle.height > G_MAXINT ||
		    cairo_region_union_rectangle (region, &rectangle) != CAIRO_STATUS_SUCCESS)
			goto invalid;
	}
	return region;

invalid:
	cairo_region_destroy (region);
	g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid document region");
	return NULL;
}

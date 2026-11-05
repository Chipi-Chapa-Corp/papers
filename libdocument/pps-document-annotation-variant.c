// SPDX-License-Identifier: GPL-2.0-or-later

#include <math.h>

#include "pps-document-annotation-private.h"
#include "pps-document-attachment-private.h"
#include "pps-document-buffer-private.h"
#include "pps-document-geometry-private.h"

static void
invalid_annotation (GError **error)
{
	if (!error || !*error)
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid document annotation");
}

static gboolean
transport_property (GParamSpec *spec)
{
	return spec && (spec->flags & G_PARAM_READWRITE) == G_PARAM_READWRITE && !(spec->flags & G_PARAM_CONSTRUCT_ONLY);
}

static GVariant *
ink_to_variant (PpsInkList *ink)
{
	GVariantBuilder paths;
	g_variant_builder_init (&paths, G_VARIANT_TYPE ("aa(dd)"));
	for (gsize i = 0; ink && i < ink->n_paths; i++) {
		GVariantBuilder points;
		g_variant_builder_init (&points, G_VARIANT_TYPE ("a(dd)"));
		for (gsize j = 0; j < ink->paths[i]->n_points; j++)
			g_variant_builder_add (&points, "(dd)", ink->paths[i]->points[j].x, ink->paths[i]->points[j].y);
		g_variant_builder_add_value (&paths, g_variant_builder_end (&points));
	}
	return g_variant_new ("(b@aa(dd))", ink != NULL, g_variant_builder_end (&paths));
}

static gboolean
ink_from_variant (GVariant *value, GValue *result)
{
	gboolean present;
	g_autoptr (GVariant) paths = NULL;
	g_autoptr (GPtrArray) decoded = g_ptr_array_new_with_free_func ((GDestroyNotify) pps_path_free);
	if (!g_variant_is_of_type (value, G_VARIANT_TYPE ("(baa(dd))")))
		return FALSE;
	g_variant_get (value, "(b@aa(dd))", &present, &paths);
	gsize count = g_variant_n_children (paths);
	if (count > G_MAXUINT || (!present && count))
		return FALSE;
	for (gsize i = 0; i < count; i++) {
		g_autoptr (GVariant) points = g_variant_get_child_value (paths, i);
		gsize n_points = g_variant_n_children (points);
		if (n_points > G_MAXUINT || n_points > G_MAXSIZE / sizeof (PpsPoint))
			return FALSE;
		g_autofree PpsPoint *array = g_new (PpsPoint, n_points);
		for (gsize j = 0; j < n_points; j++) {
			g_variant_get_child (points, j, "(dd)", &array[j].x, &array[j].y);
			if (!isfinite (array[j].x) || !isfinite (array[j].y))
				return FALSE;
		}
		g_ptr_array_add (decoded, pps_path_new_for_array (array, n_points));
	}
	if (present)
		g_value_take_boxed (result, pps_ink_list_new_for_array ((const PpsPath **) decoded->pdata, decoded->len));
	return TRUE;
}

GVariant *
_pps_annotation_property_to_variant (PpsAnnotation *annotation, const char *name, GUnixFDList *fd_list, GError **error)
{
	GParamSpec *spec = g_object_class_find_property (G_OBJECT_GET_CLASS (annotation), name);
	GValue value = G_VALUE_INIT;
	GVariant *result = NULL;
	if (!transport_property (spec) || !g_str_equal (name, spec->name))
		goto invalid;
	g_value_init (&value, spec->value_type);
	g_object_get_property (G_OBJECT (annotation), name, &value);
	GType type = spec->value_type;
	if (type == G_TYPE_STRING) {
		const char *text = g_value_get_string (&value);
		result = g_variant_new ("(bs)", text != NULL, text ? text : "");
	} else if (type == G_TYPE_BOOLEAN) {
		result = g_variant_new_boolean (g_value_get_boolean (&value));
	} else if (type == G_TYPE_DOUBLE) {
		result = g_variant_new_double (g_value_get_double (&value));
	} else if (G_TYPE_IS_ENUM (type)) {
		result = g_variant_new_int32 (g_value_get_enum (&value));
	} else if (type == GDK_TYPE_RGBA) {
		result = _pps_rgba_to_variant (g_value_get_boxed (&value));
	} else if (type == PPS_TYPE_RECTANGLE) {
		PpsRectangle *area = g_value_get_boxed (&value);
		result = g_variant_new ("(dddd)", area->x1, area->y1, area->x2, area->y2);
	} else if (type == PANGO_TYPE_FONT_DESCRIPTION) {
		PangoFontDescription *font = g_value_get_boxed (&value);
		g_autofree char *text = font ? pango_font_description_to_string (font) : NULL;
		result = g_variant_new ("(bs)", text != NULL, text ? text : "");
	} else if (type == PPS_TYPE_INK_LIST) {
		result = ink_to_variant (g_value_get_boxed (&value));
	} else if (type == PPS_TYPE_ATTACHMENT) {
		result = _pps_attachment_to_variant (g_value_get_object (&value), fd_list, error);
	}
	g_value_unset (&value);
	if (result)
		return result;
invalid:
	invalid_annotation (error);
	return NULL;
}

static gboolean
property_value (GParamSpec *spec, GVariant *value, GUnixFDList *fd_list, GValue *result, GError **error)
{
	GType type = spec->value_type;
	g_value_init (result, type);
	if ((type == G_TYPE_STRING || type == PANGO_TYPE_FONT_DESCRIPTION) &&
	    g_variant_is_of_type (value, G_VARIANT_TYPE ("(bs)"))) {
		gboolean present;
		const char *text;
		g_variant_get (value, "(b&s)", &present, &text);
		if (!present && *text)
			return FALSE;
		if (type == G_TYPE_STRING)
			g_value_set_string (result, present ? text : NULL);
		else if (present)
			g_value_take_boxed (result, pango_font_description_from_string (text));
	} else if (type == G_TYPE_BOOLEAN && g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN)) {
		g_value_set_boolean (result, g_variant_get_boolean (value));
	} else if (type == G_TYPE_DOUBLE && g_variant_is_of_type (value, G_VARIANT_TYPE_DOUBLE)) {
		double number = g_variant_get_double (value);
		if (!isfinite (number))
			return FALSE;
		g_value_set_double (result, number);
	} else if (G_TYPE_IS_ENUM (type) && g_variant_is_of_type (value, G_VARIANT_TYPE_INT32)) {
		g_value_set_enum (result, g_variant_get_int32 (value));
	} else if (type == GDK_TYPE_RGBA) {
		GdkRGBA color;
		if (!_pps_rgba_from_variant (value, &color))
			return FALSE;
		g_value_set_boxed (result, &color);
	} else if (type == PPS_TYPE_RECTANGLE) {
		PpsRectangle rectangle;
		if (!_pps_rectangle_from_variant (value, &rectangle))
			return FALSE;
		g_value_set_boxed (result, &rectangle);
	} else if (type == PPS_TYPE_INK_LIST) {
		if (!ink_from_variant (value, result))
			return FALSE;
	} else if (type == PPS_TYPE_ATTACHMENT) {
		PpsAttachment *attachment = _pps_attachment_from_variant (value, fd_list, error);
		if (!attachment && (!g_variant_is_of_type (value, G_VARIANT_TYPE_VARDICT) || g_variant_n_children (value)))
			return FALSE;
		g_value_take_object (result, attachment);
	} else {
		return FALSE;
	}
	return !g_param_value_validate (spec, result);
}

gboolean
_pps_annotation_set_property_from_variant (PpsAnnotation *annotation, const char *name, GVariant *value, GUnixFDList *fd_list, GError **error)
{
	GParamSpec *spec = g_object_class_find_property (G_OBJECT_GET_CLASS (annotation), name);
	GValue decoded = G_VALUE_INIT;
	gboolean valid = transport_property (spec) && g_str_equal (name, spec->name) && property_value (spec, value, fd_list, &decoded, error);
	if (valid) {
		GValue current = G_VALUE_INIT;
		g_value_init (&current, spec->value_type);
		g_object_get_property (G_OBJECT (annotation), name, &current);
		if (g_param_values_cmp (spec, &current, &decoded) != 0) {
			/* Native setters require a non-NULL font and markup label. */
			if ((spec->value_type == PANGO_TYPE_FONT_DESCRIPTION && !g_value_get_boxed (&decoded)) ||
			    (g_str_equal (name, "label") && !g_value_get_string (&decoded)))
				valid = FALSE;
			else
				g_object_set_property (G_OBJECT (annotation), name, &decoded);
		}
		g_value_unset (&current);
	}
	if (G_IS_VALUE (&decoded))
		g_value_unset (&decoded);
	if (!valid)
		invalid_annotation (error);
	return valid;
}

GVariant *
_pps_annotation_to_variant (PpsAnnotation *annotation, GUnixFDList *fd_list, GError **error)
{
	GVariantBuilder builder, properties;
	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_init (&properties, G_VARIANT_TYPE_VARDICT);
	guint n_specs;
	g_autofree GParamSpec **specs = g_object_class_list_properties (G_OBJECT_GET_CLASS (annotation), &n_specs);
	for (guint i = 0; i < n_specs; i++) {
		if (!transport_property (specs[i]))
			continue;
		GVariant *value = _pps_annotation_property_to_variant (annotation, specs[i]->name, fd_list, error);
		if (!value)
			goto failed;
		g_variant_builder_add (&properties, "{sv}", specs[i]->name, value);
	}
	g_variant_builder_add (&builder, "{sv}", "kind", g_variant_new_uint32 (pps_annotation_get_annotation_type (annotation)));
	g_variant_builder_add (&builder, "{sv}", "page", g_variant_new_int32 (pps_annotation_get_page_index (annotation)));
	if (PPS_IS_ANNOTATION_STAMP (annotation)) {
		cairo_surface_t *surface = pps_annotation_stamp_get_surface (PPS_ANNOTATION_STAMP (annotation));
		if (surface) {
			GVariant *buffer = _pps_document_surface_append_buffer (surface, fd_list, error);
			if (!buffer)
				goto failed;
			g_variant_builder_add (&builder, "{sv}", "surface", buffer);
		}
	}
	if (PPS_IS_ANNOTATION_INK (annotation)) {
		GVariantBuilder times;
		g_variant_builder_init (&times, G_VARIANT_TYPE ("a(udd)"));
		for (GSList *item = pps_annotation_ink_get_time_list (PPS_ANNOTATION_INK (annotation)); item; item = item->next) {
			PpsInkTime *time = item->data;
			g_variant_builder_add (&times, "(udd)", time->time, time->x, time->y);
		}
		g_variant_builder_add (&builder, "{sv}", "times", g_variant_builder_end (&times));
		g_variant_builder_add (&builder, "{sv}", "highlight", g_variant_new_boolean (pps_annotation_ink_get_highlight (PPS_ANNOTATION_INK (annotation))));
	}
	g_variant_builder_add (&builder, "{sv}", "properties", g_variant_builder_end (&properties));
	return g_variant_builder_end (&builder);
failed:
	g_variant_builder_clear (&properties);
	g_variant_builder_clear (&builder);
	return NULL;
}

PpsAnnotation *
_pps_annotation_from_variant (GVariant *value, PpsPage *page, GUnixFDList *fd_list, GError **error)
{
	const GType types[] = { G_TYPE_INVALID, PPS_TYPE_ANNOTATION_TEXT, PPS_TYPE_ANNOTATION_FREE_TEXT, PPS_TYPE_ANNOTATION_ATTACHMENT,
		                PPS_TYPE_ANNOTATION_TEXT_MARKUP, PPS_TYPE_ANNOTATION_STAMP, PPS_TYPE_ANNOTATION_INK };
	guint kind;
	int index;
	g_autoptr (PpsPage) local_page = NULL;
	g_autoptr (PpsAnnotation) annotation = NULL;
	g_autoptr (GVariant) properties = NULL;
	g_autoptr (GHashTable) seen = NULL;
	g_autofree GParamSpec **specs = NULL;
	if (!g_variant_is_of_type (value, G_VARIANT_TYPE_VARDICT) ||
	    !g_variant_lookup (value, "kind", "u", &kind) || kind == 0 || kind >= G_N_ELEMENTS (types) ||
	    !g_variant_lookup (value, "page", "i", &index) || index < 0 || (page && page->index != index))
		goto invalid;
	properties = g_variant_lookup_value (value, "properties", G_VARIANT_TYPE_VARDICT);
	if (!properties)
		goto invalid;
	if (!page) {
		local_page = pps_page_new (index);
		page = local_page;
	}
	if (kind == PPS_ANNOTATION_TYPE_INK) {
		gboolean highlight;
		if (!g_variant_lookup (value, "highlight", "b", &highlight))
			goto invalid;
		annotation = g_object_new (types[kind], "page", page, "highlight", highlight, NULL);
	} else {
		annotation = g_object_new (types[kind], "page", page, NULL);
	}
	GVariantIter iter;
	const char *name;
	GVariant *property;
	seen = g_hash_table_new (g_str_hash, g_str_equal);
	g_variant_iter_init (&iter, properties);
	while (g_variant_iter_next (&iter, "{&sv}", &name, &property)) {
		g_autoptr (GVariant) owned = property;
		if (g_hash_table_contains (seen, name) || !_pps_annotation_set_property_from_variant (annotation, name, property, fd_list, error))
			goto invalid;
		g_hash_table_add (seen, (char *) name);
	}
	guint n_specs, expected = 0;
	specs = g_object_class_list_properties (G_OBJECT_GET_CLASS (annotation), &n_specs);
	for (guint i = 0; i < n_specs; i++)
		expected += transport_property (specs[i]);
	if (g_hash_table_size (seen) != expected)
		goto invalid;
	if (kind == PPS_ANNOTATION_TYPE_STAMP) {
		g_autoptr (GVariant) buffer = g_variant_lookup_value (value, "surface", NULL);
		if (buffer) {
			cairo_surface_t *surface = _pps_document_surface_from_buffer (buffer, fd_list, error);
			if (!surface)
				goto invalid;
			pps_annotation_stamp_set_surface (PPS_ANNOTATION_STAMP (annotation), surface);
			cairo_surface_destroy (surface);
		}
	}
	if (kind == PPS_ANNOTATION_TYPE_INK) {
		g_autoptr (GVariant) times = g_variant_lookup_value (value, "times", G_VARIANT_TYPE ("a(udd)"));
		GSList *list = NULL;
		if (!times)
			goto invalid;
		for (gsize i = 0; i < g_variant_n_children (times); i++) {
			PpsInkTime time;
			g_variant_get_child (times, i, "(udd)", &time.time, &time.x, &time.y);
			if (!isfinite (time.x) || !isfinite (time.y)) {
				g_slist_free_full (list, g_free);
				goto invalid;
			}
			list = g_slist_prepend (list, pps_ink_time_copy (&time));
		}
		pps_annotation_ink_set_time_list (PPS_ANNOTATION_INK (annotation), g_slist_reverse (list));
	}
	return g_steal_pointer (&annotation);
invalid:
	invalid_annotation (error);
	return NULL;
}

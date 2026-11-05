// SPDX-License-Identifier: GPL-2.0-or-later

#include <math.h>

#include "pps-document-annotation-private.h"
#include "pps-document-attachment-private.h"

static void
roundtrip (void)
{
	g_autoptr (PpsPage) page = pps_page_new (2);
	g_autoptr (GPtrArray) annotations = g_ptr_array_new_with_free_func (g_object_unref);
	g_ptr_array_add (annotations, pps_annotation_text_new (page));
	g_ptr_array_add (annotations, pps_annotation_free_text_new (page));
	g_autoptr (PpsAttachment) attachment = pps_attachment_new ("file.bin", NULL, NULL, NULL, 3, g_memdup2 ("a\0b", 3));
	g_ptr_array_add (annotations, pps_annotation_attachment_new (page, attachment));
	for (int type = 0; type <= PPS_ANNOTATION_TEXT_MARKUP_SQUIGGLY; type++)
		g_ptr_array_add (annotations, pps_annotation_text_markup_new (page, type));
	g_ptr_array_add (annotations, pps_annotation_stamp_new (page));
	g_ptr_array_add (annotations, pps_annotation_ink_new (page));
	g_ptr_array_add (annotations, pps_annotation_ink_new_highlight (page));
	for (guint i = 0; i < annotations->len; i++) {
		PpsAnnotation *original = annotations->pdata[i];
		GdkRGBA color = { .2, .4, .6, .8 };
		PpsRectangle area = { 1.25, 2.5, 80, 90 };
		g_object_set (original, "contents", "Contents α", "name", "annotation-name", "modified", "2024-03-24",
		              "hidden", TRUE, "border-width", 2.25, "area", &area, "rgba", &color, NULL);
		if (PPS_IS_ANNOTATION_MARKUP (original))
			g_object_set (original, "label", "Author α", "opacity", .55, "has-popup", TRUE,
			              "rectangle", &area, "popup-is-open", TRUE, NULL);
		if (PPS_IS_ANNOTATION_TEXT (original))
			g_object_set (original, "icon", PPS_ANNOTATION_TEXT_ICON_KEY, "is-open", TRUE, NULL);
		if (PPS_IS_ANNOTATION_FREE_TEXT (original)) {
			g_autoptr (PangoFontDescription) font = pango_font_description_from_string ("Sans Bold 17");
			pango_font_description_set_absolute_size (font, 17 * PANGO_SCALE);
			pango_font_description_set_variations (font, "wght=625");
			g_object_set (original, "font-desc", font, "font-rgba", &color, NULL);
		}
		if (PPS_IS_ANNOTATION_INK (original)) {
			PpsPoint points[] = { { .25, .5 }, { 10, -20 }, { 80, 90 } };
			PpsPath first = { points, 3 }, second = { points + 1, 1 }, empty = { NULL, 0 };
			gsize count;
			g_autofree PpsPoint *empty_copy = pps_path_copy_array (&empty, &count);
			g_assert_null (empty_copy);
			g_assert_cmpuint (count, ==, 0);
			PpsInkList *ink = pps_ink_list_new_for_array ((const PpsPath *[]){ &first, &second, &empty }, 3);
			pps_annotation_ink_set_ink_list (PPS_ANNOTATION_INK (original), ink);
			GSList *times = NULL;
			times = g_slist_append (times, pps_ink_time_copy (&(PpsInkTime){ 5, .25, .5 }));
			times = g_slist_append (times, pps_ink_time_copy (&(PpsInkTime){ 7, 10, -20 }));
			pps_annotation_ink_set_time_list (PPS_ANNOTATION_INK (original), times);
		}
		if (PPS_IS_ANNOTATION_STAMP (original)) {
			cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 3, 2);
			cairo_t *cr = cairo_create (surface);
			cairo_set_source_rgba (cr, .2, .4, .6, .8);
			cairo_paint (cr);
			cairo_destroy (cr);
			pps_annotation_stamp_set_surface (PPS_ANNOTATION_STAMP (original), surface);
			cairo_surface_destroy (surface);
		}
		g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
		g_autoptr (GError) error = NULL;
		g_autoptr (GVariant) value = g_variant_ref_sink (_pps_annotation_to_variant (original, descriptors, &error));
		g_assert_no_error (error);
		g_autoptr (PpsAnnotation) copy = _pps_annotation_from_variant (value, NULL, descriptors, &error);
		g_assert_no_error (error);
		g_assert_nonnull (copy);
		g_assert_cmpuint (G_OBJECT_TYPE (copy), ==, G_OBJECT_TYPE (original));
		g_assert_cmpuint (pps_annotation_get_page_index (copy), ==, 2);
		g_assert_null (pps_annotation_get_page (copy)->backend_page);
		guint n_specs;
		g_autofree GParamSpec **specs = g_object_class_list_properties (G_OBJECT_GET_CLASS (original), &n_specs);
		for (guint j = 0; j < n_specs; j++) {
			if ((specs[j]->flags & G_PARAM_READWRITE) != G_PARAM_READWRITE ||
			    (specs[j]->flags & G_PARAM_CONSTRUCT_ONLY) || specs[j]->value_type == PPS_TYPE_ATTACHMENT)
				continue;
			g_autoptr (GVariant) before = g_variant_ref_sink (_pps_annotation_property_to_variant (original, specs[j]->name, NULL, &error));
			g_assert_no_error (error);
			g_autoptr (GVariant) after = g_variant_ref_sink (_pps_annotation_property_to_variant (copy, specs[j]->name, NULL, &error));
			g_assert_no_error (error);
			g_assert_true (g_variant_equal (before, after));
		}
		if (PPS_IS_ANNOTATION_INK (copy)) {
			g_assert_cmpint (pps_annotation_ink_get_highlight (PPS_ANNOTATION_INK (copy)), ==,
			                 pps_annotation_ink_get_highlight (PPS_ANNOTATION_INK (original)));
			GSList *times = pps_annotation_ink_get_time_list (PPS_ANNOTATION_INK (copy));
			g_assert_cmpuint (g_slist_length (times), ==, 2);
			g_assert_cmpuint (((PpsInkTime *) times->data)->time, ==, 5);
			g_assert_cmpfloat (((PpsInkTime *) times->next->data)->y, ==, -20);
		}
		if (PPS_IS_ANNOTATION_ATTACHMENT (copy)) {
			PpsAttachment *payload = pps_annotation_attachment_get_attachment (PPS_ANNOTATION_ATTACHMENT (copy));
			g_assert_cmpstr (pps_attachment_get_name (payload), ==, "file.bin");
			g_autoptr (GBytes) bytes = _pps_attachment_get_data (payload);
			g_assert_cmpmem (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes), "a\0b", 3);
		}
		if (PPS_IS_ANNOTATION_STAMP (copy)) {
			cairo_surface_t *before = pps_annotation_stamp_get_surface (PPS_ANNOTATION_STAMP (original));
			cairo_surface_t *after = pps_annotation_stamp_get_surface (PPS_ANNOTATION_STAMP (copy));
			g_clear_object (&descriptors);
			g_assert_cmpmem (cairo_image_surface_get_data (before), 24, cairo_image_surface_get_data (after), 24);
		}
	}
}

static void
empty_values (void)
{
	g_autoptr (PpsPage) page = pps_page_new (0);
	GType types[] = { PPS_TYPE_ANNOTATION_TEXT, PPS_TYPE_ANNOTATION_FREE_TEXT, PPS_TYPE_ANNOTATION_ATTACHMENT,
		          PPS_TYPE_ANNOTATION_TEXT_MARKUP, PPS_TYPE_ANNOTATION_STAMP, PPS_TYPE_ANNOTATION_INK };
	for (guint i = 0; i < G_N_ELEMENTS (types); i++) {
		g_autoptr (PpsAnnotation) original = g_object_new (types[i], "page", page, NULL);
		if (i % 2)
			pps_annotation_set_contents (original, NULL);
		g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
		g_autoptr (GError) error = NULL;
		g_autoptr (GVariant) value = g_variant_ref_sink (_pps_annotation_to_variant (original, descriptors, &error));
		g_assert_no_error (error);
		g_autoptr (PpsAnnotation) copy = _pps_annotation_from_variant (value, page, descriptors, &error);
		g_assert_no_error (error);
		g_assert_nonnull (copy);
		g_assert_true (pps_annotation_get_page (copy) == page);
		g_assert_cmpstr (pps_annotation_get_contents (copy), ==, pps_annotation_get_contents (original));
		g_assert_null (pps_annotation_get_name (copy));
		g_assert_cmpint (g_unix_fd_list_get_length (descriptors), ==, 0);
	}
}

typedef struct {
	guint count;
	GValue previous;
} PropertyChange;

static void
contents_changed (PpsAnnotation *annotation, GParamSpec *spec, PropertyChange *change)
{
	change->count++;
	pps_annotation_get_value_last_property (annotation, &change->previous);
}

static void
property_notifications_and_undo (void)
{
	g_autoptr (PpsPage) page = pps_page_new (0);
	g_autoptr (PpsAnnotation) annotation = pps_annotation_text_new (page);
	pps_annotation_set_contents (annotation, "Before");
	PropertyChange change = { .previous = G_VALUE_INIT };
	g_value_init (&change.previous, G_TYPE_STRING);
	g_signal_connect (annotation, "notify::contents", G_CALLBACK (contents_changed), &change);
	g_autoptr (GVariant) after = g_variant_ref_sink (g_variant_new ("(bs)", TRUE, "After"));
	g_autoptr (GError) error = NULL;
	g_assert_true (_pps_annotation_set_property_from_variant (annotation, "contents", after, NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpuint (change.count, ==, 1);
	g_assert_cmpstr (g_value_get_string (&change.previous), ==, "Before");
	g_assert_cmpstr (pps_annotation_get_contents (annotation), ==, "After");
	g_autoptr (GVariant) before = g_variant_ref_sink (g_variant_new ("(bs)", TRUE, g_value_get_string (&change.previous)));
	g_assert_true (_pps_annotation_set_property_from_variant (annotation, "contents", before, NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpuint (change.count, ==, 2);
	g_assert_cmpstr (g_value_get_string (&change.previous), ==, "After");
	g_assert_cmpstr (pps_annotation_get_contents (annotation), ==, "Before");
	g_value_unset (&change.previous);
}

static void
invalid_properties (void)
{
	g_autoptr (PpsPage) page = pps_page_new (0);
	g_autoptr (PpsAnnotation) annotation = pps_annotation_text_new (page);
	const char *names[] = { "page", "can-have-popup", "missing", "contents", "border-width", "opacity", "rgba", "area", "icon", "popup_is_open" };
	GVariant *values[] = { g_variant_new_int32 (1), g_variant_new_boolean (TRUE), g_variant_new_boolean (FALSE),
		               g_variant_new_int32 (1), g_variant_new_double (-1), g_variant_new_double (NAN),
		               g_variant_new ("(dddd)", 0., 0., 0., INFINITY), g_variant_new ("(dddd)", 0., 0., NAN, 1.),
		               g_variant_new_int32 (G_MAXINT), g_variant_new_boolean (TRUE) };
	for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
		g_autoptr (GVariant) value = g_variant_ref_sink (values[i]);
		g_autoptr (GError) error = NULL;
		g_assert_false (_pps_annotation_set_property_from_variant (annotation, names[i], value, NULL, &error));
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	}
	g_assert_cmpstr (pps_annotation_get_contents (annotation), ==, "");
	g_assert_cmpfloat (pps_annotation_get_border_width (annotation), ==, 0);
	g_assert_cmpint (pps_annotation_text_get_icon (PPS_ANNOTATION_TEXT (annotation)), ==, PPS_ANNOTATION_TEXT_ICON_NOTE);
}

static void
reject_annotation (GVariant *value, GUnixFDList *descriptors)
{
	g_autoptr (GVariant) owned = g_variant_take_ref (value);
	g_autoptr (GError) error = NULL;
	g_assert_null (_pps_annotation_from_variant (owned, NULL, descriptors, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
invalid_descriptors (void)
{
	g_autoptr (PpsPage) page = pps_page_new (0);
	g_autoptr (PpsAnnotation) annotation = pps_annotation_ink_new (page);
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	g_autoptr (GError) error = NULL;
	g_autoptr (GVariant) valid = g_variant_ref_sink (_pps_annotation_to_variant (annotation, descriptors, &error));
	g_assert_no_error (error);
	const char *keys[] = { "kind", "kind", "page", "properties", "times" };
	GVariant *values[] = { g_variant_new_uint32 (0), g_variant_new_uint32 (99), g_variant_new_int32 (-1),
		               g_variant_new_boolean (FALSE), g_variant_new_parsed ("[(uint32 1, 0.0, 0.0), (2, nan, 0.0)]") };
	for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
		GVariantDict dictionary;
		g_variant_dict_init (&dictionary, valid);
		g_variant_dict_insert_value (&dictionary, keys[i], values[i]);
		reject_annotation (g_variant_dict_end (&dictionary), descriptors);
	}
	g_autoptr (GVariant) properties = g_variant_lookup_value (valid, "properties", G_VARIANT_TYPE_VARDICT);
	GVariantDict nested, dictionary;
	g_variant_dict_init (&nested, properties);
	g_variant_dict_remove (&nested, "contents");
	g_variant_dict_init (&dictionary, valid);
	g_variant_dict_insert_value (&dictionary, "properties", g_variant_dict_end (&nested));
	reject_annotation (g_variant_dict_end (&dictionary), descriptors);
	g_variant_dict_init (&nested, properties);
	g_variant_dict_insert_value (&nested, "ink-list", g_variant_new_parsed ("(true, [[(0.0, 0.0)], [(nan, 0.0)]])"));
	g_variant_dict_init (&dictionary, valid);
	g_variant_dict_insert_value (&dictionary, "properties", g_variant_dict_end (&nested));
	reject_annotation (g_variant_dict_end (&dictionary), descriptors);
	GVariantBuilder duplicates;
	g_variant_builder_init (&duplicates, G_VARIANT_TYPE_VARDICT);
	for (int repeat = 0; repeat < 2; repeat++) {
		GVariantIter iter;
		const char *name;
		GVariant *property;
		g_variant_iter_init (&iter, properties);
		while (g_variant_iter_next (&iter, "{&sv}", &name, &property)) {
			g_variant_builder_add (&duplicates, "{sv}", name, property);
			g_variant_unref (property);
		}
	}
	g_variant_dict_init (&dictionary, valid);
	g_variant_dict_insert_value (&dictionary, "properties", g_variant_builder_end (&duplicates));
	reject_annotation (g_variant_dict_end (&dictionary), descriptors);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/document-annotation/roundtrip", roundtrip);
	g_test_add_func ("/document-annotation/empty-values", empty_values);
	g_test_add_func ("/document-annotation/notifications-and-undo", property_notifications_and_undo);
	g_test_add_func ("/document-annotation/invalid-properties", invalid_properties);
	g_test_add_func ("/document-annotation/invalid-descriptors", invalid_descriptors);
	return g_test_run ();
}

// SPDX-License-Identifier: GPL-2.0-or-later

#include <math.h>

#include "pps-document-geometry-private.h"
#include "pps-link-dest.h"

static void
destination_right_property (void)
{
	g_autoptr (PpsLinkDest) dest = pps_link_dest_new_fitr (0, 1, 2, 30, 40);
	double right;
	g_object_get (dest, "right", &right, NULL);
	g_assert_cmpfloat (right, ==, 30);
	g_assert_cmpfloat (right, ==, pps_link_dest_get_right (dest));
}

static void
rectangles (void)
{
	PpsRectangle source[] = { { 1, 2, 3, 4 }, { 5.5, 6.5, -7.5, -8.5 } };
	g_autoptr (GVariant) value = g_variant_ref_sink (_pps_rectangles_to_variant (source, 2));
	g_autofree PpsRectangle *result = NULL;
	g_autoptr (GError) error = NULL;
	guint count;

	g_assert_true (_pps_rectangles_from_variant (value, &result, &count, &error));
	g_assert_no_error (error);
	g_assert_cmpuint (count, ==, 2);
	g_assert_cmpmem (source, sizeof (source), result, count * sizeof (PpsRectangle));
	g_clear_pointer (&value, g_variant_unref);
	value = g_variant_ref_sink (g_variant_new_parsed ("[(0.0, 0.0, 1.0, 2.0)]"));
	PpsRectangle rectangle;
	g_assert_false (_pps_rectangle_from_variant (value, &rectangle));
}

static void
regions (void)
{
	cairo_rectangle_int_t rectangles[] = { { 1, 2, 20, 30 }, { 10, 15, 20, 40 } };
	cairo_region_t *original = cairo_region_create_rectangles (rectangles, G_N_ELEMENTS (rectangles));
	g_autoptr (GVariant) value = g_variant_ref_sink (_pps_region_to_variant (original));
	g_autoptr (GError) error = NULL;
	cairo_region_t *result = _pps_region_from_variant (value, &error);

	g_assert_no_error (error);
	g_assert_true (cairo_region_equal (original, result));
	cairo_region_destroy (original);
	cairo_region_destroy (result);
}

static void
invalid_geometry (void)
{
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(dddd)"));
	g_variant_builder_add (&builder, "(dddd)", 0., 0., NAN, 1.);
	g_autoptr (GVariant) rectangles = g_variant_ref_sink (g_variant_builder_end (&builder));
	g_autoptr (GError) error = NULL;
	g_autofree PpsRectangle *areas = NULL;
	guint count;
	g_assert_false (_pps_rectangles_from_variant (rectangles, &areas, &count, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_clear_error (&error);
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(iiii)"));
	g_variant_builder_add (&builder, "(iiii)", G_MAXINT, 0, 2, 1);
	g_autoptr (GVariant) region = g_variant_ref_sink (g_variant_builder_end (&builder));
	g_assert_null (_pps_region_from_variant (region, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/document-geometry/rectangles", rectangles);
	g_test_add_func ("/document-geometry/regions", regions);
	g_test_add_func ("/document-geometry/invalid", invalid_geometry);
	g_test_add_func ("/document-geometry/destination-right", destination_right_property);
	return g_test_run ();
}

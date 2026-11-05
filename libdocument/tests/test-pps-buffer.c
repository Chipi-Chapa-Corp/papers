// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <fcntl.h>
#include <math.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pps-document-buffer-private.h"

static void
roundtrip (void)
{
	cairo_format_t formats[] = {
		CAIRO_FORMAT_ARGB32, CAIRO_FORMAT_RGB24, CAIRO_FORMAT_A8,
		CAIRO_FORMAT_A1, CAIRO_FORMAT_RGB16_565, CAIRO_FORMAT_RGB30
	};

	for (guint i = 0; i < G_N_ELEMENTS (formats); i++) {
		g_autoptr (GUnixFDList) fd_list = NULL;
		g_autoptr (GVariant) buffer = NULL;
		g_autoptr (GError) error = NULL;
		cairo_surface_t *original = cairo_image_surface_create (formats[i], 16, 8);
		cairo_t *cr = cairo_create (original);
		cairo_surface_t *first, *second;
		double sx, sy, ox, oy;
		gsize size = 8 * cairo_image_surface_get_stride (original);

		cairo_set_source_rgb (cr, 1, 0, 0);
		cairo_paint (cr);
		cairo_destroy (cr);
		cairo_surface_set_device_scale (original, 2, 3);
		cairo_surface_set_device_offset (original, 4, -5);
		buffer = g_variant_ref_sink (_pps_document_surface_to_buffer (original, &fd_list, &error));
		g_assert_no_error (error);
		first = _pps_document_surface_from_buffer (buffer, fd_list, &error);
		g_assert_no_error (error);
		g_assert_nonnull (first);
		g_assert_cmpmem (cairo_image_surface_get_data (original), size,
		                 cairo_image_surface_get_data (first), size);
		cairo_surface_get_device_scale (first, &sx, &sy);
		cairo_surface_get_device_offset (first, &ox, &oy);
		g_assert_cmpfloat (sx, ==, 2);
		g_assert_cmpfloat (sy, ==, 3);
		g_assert_cmpfloat (ox, ==, 4);
		g_assert_cmpfloat (oy, ==, -5);

		/* Local post-processing must not mutate the transferred buffer. */
		cr = cairo_create (first);
		cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
		cairo_paint (cr);
		cairo_destroy (cr);
		second = _pps_document_surface_from_buffer (buffer, fd_list, &error);
		g_assert_no_error (error);
		cairo_surface_flush (first);
		g_assert_cmpint (memcmp (cairo_image_surface_get_data (first),
		                         cairo_image_surface_get_data (second), size),
		                 !=, 0);
		g_assert_cmpmem (cairo_image_surface_get_data (original), size,
		                 cairo_image_surface_get_data (second), size);
		/* Mappings outlive the file descriptors and original image. */
		g_clear_object (&fd_list);
		cairo_surface_destroy (original);
		cr = cairo_create (second);
		cairo_paint (cr);
		g_assert_cmpint (cairo_status (cr), ==, CAIRO_STATUS_SUCCESS);
		cairo_destroy (cr);
		cairo_surface_destroy (first);
		cairo_surface_destroy (second);
	}
}

static void
reject_buffer (GVariant *buffer, GUnixFDList *fd_list)
{
	g_autoptr (GVariant) owned = g_variant_ref_sink (buffer);
	g_autoptr (GError) error = NULL;
	cairo_surface_t *surface = _pps_document_surface_from_buffer (owned, fd_list, &error);

	g_assert_null (surface);
	g_assert_nonnull (error);
}

static void
invalid_metadata (void)
{
	g_autoptr (GUnixFDList) fd_list = NULL;
	g_autoptr (GVariant) buffer = NULL;
	g_autoptr (GError) error = NULL;
	cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 16, 8);

	buffer = g_variant_ref_sink (_pps_document_surface_to_buffer (surface, &fd_list, &error));
	g_assert_no_error (error);
	cairo_surface_destroy (surface);
	reject_buffer (g_variant_new_int32 (0), fd_list);
	reject_buffer (g_variant_ref (buffer), NULL);
	reject_buffer (g_variant_new ("(hiiiidddd)", 0, -1, 16, 8, 64, 1., 1., 0., 0.), fd_list);
	reject_buffer (g_variant_new ("(hiiiidddd)", 0, 0, -1, 8, 64, 1., 1., 0., 0.), fd_list);
	reject_buffer (g_variant_new ("(hiiiidddd)", 0, 0, 16, 8, 63, 1., 1., 0., 0.), fd_list);
	reject_buffer (g_variant_new ("(hiiiidddd)", 0, 0, 16, 9, 64, 1., 1., 0., 0.), fd_list);
	reject_buffer (g_variant_new ("(hiiiidddd)", 0, 0, 16, 8, 64, NAN, 1., 0., 0.), fd_list);
	reject_buffer (g_variant_new ("(hiiiidddd)", 0, 0, 16, 8, 64, 1., -1., 0., 0.), fd_list);
	reject_buffer (g_variant_new ("(hiiiidddd)", 0, 0, 16, 8, 64, 1., 1., INFINITY, 0.), fd_list);
	reject_buffer (g_variant_new ("(hiiiidddd)", 1, 0, 16, 8, 64, 1., 1., 0., 0.), fd_list);
	reject_buffer (g_variant_new ("(hiiiidddd)", -1, 0, 16, 8, 64, 1., 1., 0., 0.), fd_list);
}

static void
reject_mutable_file (void)
{
	g_autoptr (GUnixFDList) fd_list = g_unix_fd_list_new ();
	g_autoptr (GError) error = NULL;
	int fd = memfd_create ("mutable-render-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	int handle;

	g_assert_cmpint (fd, >=, 0);
	g_assert_cmpint (ftruncate (fd, 512), ==, 0);
	handle = g_unix_fd_list_append (fd_list, fd, &error);
	g_assert_no_error (error);
	close (fd);
	reject_buffer (g_variant_new ("(hiiiidddd)", handle, 0, 16, 8, 64, 1., 1., 0., 0.), fd_list);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/document-buffer/roundtrip", roundtrip);
	g_test_add_func ("/document-buffer/invalid-metadata", invalid_metadata);
	g_test_add_func ("/document-buffer/reject-mutable-file", reject_mutable_file);
	return g_test_run ();
}

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
	g_autoptr (GVariant) owned = g_variant_take_ref (buffer);
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
	g_autoptr (GBytes) bytes = _pps_document_bytes_from_buffer (handle, 512, fd_list, &error);
	g_assert_null (bytes);
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
bytes_and_image_batch (void)
{
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	g_autoptr (GBytes) original = g_bytes_new_static ("a\0b\xff", 4);
	g_autoptr (GBytes) empty = g_bytes_new_static (NULL, 0);
	g_autoptr (GError) error = NULL;
	int first = _pps_document_bytes_to_buffer (original, descriptors, &error);
	g_assert_no_error (error);
	int second = _pps_document_bytes_to_buffer (empty, descriptors, &error);
	g_assert_no_error (error);
	cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 2, 2);
	g_autoptr (GVariant) image = g_variant_ref_sink (_pps_document_surface_append_buffer (surface, descriptors, &error));
	cairo_surface_destroy (surface);
	g_assert_no_error (error);
	g_assert_cmpint (g_unix_fd_list_get_length (descriptors), ==, 3);
	g_assert_cmpint (first, ==, 0);
	g_assert_cmpint (second, ==, 1);
	g_autoptr (GBytes) copy = _pps_document_bytes_from_buffer (first, 4, descriptors, &error);
	g_assert_no_error (error);
	g_autoptr (GBytes) empty_copy = _pps_document_bytes_from_buffer (second, 0, descriptors, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (g_bytes_get_size (empty_copy), ==, 0);
	surface = _pps_document_surface_from_buffer (image, descriptors, &error);
	g_assert_no_error (error);
	g_assert_nonnull (surface);
	g_assert_cmpint (cairo_image_surface_get_width (surface), ==, 2);
	cairo_surface_destroy (surface);
	g_clear_object (&descriptors);
	/* Byte mappings, including empty payloads, outlive the descriptor batch. */
	g_assert_true (g_bytes_equal (original, copy));
	g_assert_true (g_bytes_equal (empty, empty_copy));
}

static void
invalid_bytes (void)
{
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	g_autoptr (GBytes) original = g_bytes_new_static ("data", 4);
	g_autoptr (GError) error = NULL;
	g_assert_cmpint (_pps_document_bytes_to_buffer (original, descriptors, &error), ==, 0);
	g_assert_no_error (error);
	const struct {
		int handle;
		guint64 size;
	} invalid[] = { { -1, 4 }, { 1, 4 }, { 0, 0 }, { 0, 5 }, { 0, G_MAXUINT64 } };
	for (guint i = 0; i < G_N_ELEMENTS (invalid); i++) {
		g_autoptr (GBytes) bytes = _pps_document_bytes_from_buffer (invalid[i].handle, invalid[i].size, descriptors, &error);
		g_assert_null (bytes);
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
		g_clear_error (&error);
	}
	g_assert_null (_pps_document_bytes_from_buffer (0, 4, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
pixbuf_roundtrip (void)
{
	for (guint alpha = 0; alpha <= 1; alpha++) {
		g_autoptr (GdkPixbuf) original = gdk_pixbuf_new (GDK_COLORSPACE_RGB, alpha, 8, 3, 2);
		gdk_pixbuf_fill (original, 0x2793df80);
		gsize size = gdk_pixbuf_get_byte_length (original);
		g_autoptr (GUnixFDList) descriptors = NULL;
		g_autoptr (GError) error = NULL;
		g_autoptr (GVariant) buffer = g_variant_ref_sink (_pps_document_pixbuf_to_buffer (original, &descriptors, &error));
		g_assert_no_error (error);
		g_autoptr (GdkPixbuf) first = _pps_document_pixbuf_from_buffer (buffer, descriptors, &error);
		g_assert_no_error (error);
		g_assert_nonnull (first);
		g_assert_cmpint (gdk_pixbuf_get_has_alpha (first), ==, alpha);
		g_assert_cmpint (gdk_pixbuf_get_rowstride (first), ==, gdk_pixbuf_get_rowstride (original));
		g_assert_cmpuint (gdk_pixbuf_get_byte_length (first), ==, size);
		g_assert_cmpmem (gdk_pixbuf_read_pixels (first), size, gdk_pixbuf_read_pixels (original), size);
		gdk_pixbuf_get_pixels (first)[0] ^= 0xff;
		g_autoptr (GdkPixbuf) second = _pps_document_pixbuf_from_buffer (buffer, descriptors, &error);
		g_assert_no_error (error);
		g_clear_object (&descriptors);
		g_assert_cmpmem (gdk_pixbuf_read_pixels (second), size, gdk_pixbuf_read_pixels (original), size);
		g_clear_object (&original);
		g_assert_cmphex (gdk_pixbuf_read_pixels (second)[0], ==, 0x27);
	}
}

static void
invalid_pixbuf (void)
{
	g_autoptr (GdkPixbuf) pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 3, 2);
	g_autoptr (GUnixFDList) descriptors = NULL;
	g_autoptr (GError) error = NULL;
	g_autoptr (GVariant) original = g_variant_ref_sink (_pps_document_pixbuf_to_buffer (pixbuf, &descriptors, &error));
	g_assert_no_error (error);
	const struct {
		int handle, width, height, stride;
	} invalid[] = {
		{ 0, 0, 2, 12 },
		{ 0, -1, 2, 12 },
		{ 0, 3, 0, 12 },
		{ 0, 3, -1, 12 },
		{ 0, 3, 2, 11 },
		{ 0, 3, 3, 12 },
		{ 0, G_MAXINT, 2, 12 },
		{ -1, 3, 2, 12 },
		{ 1, 3, 2, 12 },
	};
	for (guint i = 0; i < G_N_ELEMENTS (invalid); i++) {
		g_autoptr (GVariant) value = g_variant_ref_sink (g_variant_new ("(hiiib)", invalid[i].handle, invalid[i].width,
		                                                                invalid[i].height, invalid[i].stride, TRUE));
		g_autoptr (GdkPixbuf) copy = _pps_document_pixbuf_from_buffer (value, descriptors, &error);
		g_assert_null (copy);
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
		g_clear_error (&error);
	}
	g_assert_null (_pps_document_pixbuf_from_buffer (original, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_clear_error (&error);
	g_autoptr (GVariant) wrong_type = g_variant_ref_sink (g_variant_new_int32 (0));
	g_assert_null (_pps_document_pixbuf_from_buffer (wrong_type, descriptors, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/document-buffer/roundtrip", roundtrip);
	g_test_add_func ("/document-buffer/invalid-metadata", invalid_metadata);
	g_test_add_func ("/document-buffer/reject-mutable-file", reject_mutable_file);
	g_test_add_func ("/document-buffer/bytes-and-image-batch", bytes_and_image_batch);
	g_test_add_func ("/document-buffer/invalid-bytes", invalid_bytes);
	g_test_add_func ("/document-buffer/pixbuf-roundtrip", pixbuf_roundtrip);
	g_test_add_func ("/document-buffer/invalid-pixbuf", invalid_pixbuf);
	return g_test_run ();
}

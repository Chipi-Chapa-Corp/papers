// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pps-document-buffer-private.h"

static cairo_user_data_key_t mapping_key;

static void
buffer_error (GError **error)
{
	int saved_errno = errno;
	g_set_error_literal (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
	                     g_strerror (saved_errno));
}

static int
append_data (const guint8 *data, gsize size, GUnixFDList *fd_list, GError **error)
{
	int fd, handle;
	fd = memfd_create ("papers-buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd == -1) {
		buffer_error (error);
		return -1;
	}
	for (gsize offset = 0; offset < size;) {
		ssize_t count = write (fd, data + offset, size - offset);
		if (count == -1 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (count == 0)
				errno = EIO;
			buffer_error (error);
			close (fd);
			return -1;
		}
		offset += count;
	}
	if (fcntl (fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) == -1) {
		buffer_error (error);
		close (fd);
		return -1;
	}
	handle = g_unix_fd_list_append (fd_list, fd, error);
	close (fd);
	if (handle == -1)
		return -1;
	return handle;
}

int
_pps_document_bytes_to_buffer (GBytes *bytes, GUnixFDList *fd_list, GError **error)
{
	gsize size;
	const guint8 *data = g_bytes_get_data (bytes, &size);
	return append_data (data, size, fd_list, error);
}

static GMappedFile *
map_buffer (int handle, guint64 size, GUnixFDList *fd_list, gboolean writable, GError **error)
{
	int fd, seals;
	struct stat statbuf;
	if (!fd_list || handle < 0 || handle >= g_unix_fd_list_get_length (fd_list) || size > G_MAXSIZE)
		goto invalid;
	fd = g_unix_fd_list_get (fd_list, handle, error);
	if (fd == -1)
		return NULL;
	seals = fcntl (fd, F_GET_SEALS);
	if (fstat (fd, &statbuf) == -1 || !S_ISREG (statbuf.st_mode) ||
	    statbuf.st_size < 0 || (guint64) statbuf.st_size != size ||
	    seals == -1 || (seals & (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE)) != (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE)) {
		close (fd);
		goto invalid;
	}
	GMappedFile *mapping = g_mapped_file_new_from_fd (fd, writable, error);
	close (fd);
	return mapping;
invalid:
	g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid sealed document buffer");
	return NULL;
}

GBytes *
_pps_document_bytes_from_buffer (int handle, guint64 size, GUnixFDList *fd_list, GError **error)
{
	g_autoptr (GMappedFile) mapping = map_buffer (handle, size, fd_list, FALSE, error);
	return mapping ? g_mapped_file_get_bytes (mapping) : NULL;
}

GVariant *
_pps_document_surface_append_buffer (cairo_surface_t *surface,
                                     GUnixFDList *fd_list,
                                     GError **error)
{
	cairo_status_t status = surface ? cairo_surface_status (surface) : CAIRO_STATUS_NULL_POINTER;
	int handle, width, height, stride;
	gsize size;
	double sx, sy, ox, oy;

	if (status != CAIRO_STATUS_SUCCESS ||
	    cairo_surface_get_type (surface) != CAIRO_SURFACE_TYPE_IMAGE) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		             "Cannot transfer rendered surface: %s",
		             status == CAIRO_STATUS_SUCCESS ? "not an image" : cairo_status_to_string (status));
		return NULL;
	}
	width = cairo_image_surface_get_width (surface);
	height = cairo_image_surface_get_height (surface);
	stride = cairo_image_surface_get_stride (surface);
	if (width <= 0 || height <= 0 || stride <= 0 ||
	    (guint64) stride * height > G_MAXSIZE) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		                     "Rendered surface has invalid dimensions");
		return NULL;
	}
	size = (gsize) stride * height;
	cairo_surface_flush (surface);
	handle = append_data (cairo_image_surface_get_data (surface), size, fd_list, error);
	if (handle == -1)
		return NULL;
	cairo_surface_get_device_scale (surface, &sx, &sy);
	cairo_surface_get_device_offset (surface, &ox, &oy);
	return g_variant_new ("(hiiiidddd)", handle,
	                      cairo_image_surface_get_format (surface),
	                      width, height, stride, sx, sy, ox, oy);
}

GVariant *
_pps_document_surface_to_buffer (cairo_surface_t *surface, GUnixFDList **fd_list, GError **error)
{
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	GVariant *buffer = _pps_document_surface_append_buffer (surface, descriptors, error);
	if (buffer)
		*fd_list = g_steal_pointer (&descriptors);
	return buffer;
}

cairo_surface_t *
_pps_document_surface_from_buffer (GVariant *buffer,
                                   GUnixFDList *fd_list,
                                   GError **error)
{
	g_autoptr (GMappedFile) mapping = NULL;
	int handle, format, width, height, stride, minimum_stride;
	double sx, sy, ox, oy;
	cairo_surface_t *surface;
	cairo_status_t status;

	if (!buffer || !fd_list || !g_variant_is_of_type (buffer, PPS_DOCUMENT_BUFFER_TYPE))
		goto invalid;
	g_variant_get (buffer, "(hiiiidddd)", &handle, &format, &width, &height,
	               &stride, &sx, &sy, &ox, &oy);
	if (handle < 0 || handle >= g_unix_fd_list_get_length (fd_list))
		goto invalid;
	minimum_stride = cairo_format_stride_for_width (format, width);
	if (width <= 0 || height <= 0 || minimum_stride <= 0 ||
	    stride < minimum_stride || stride % 4 != 0 ||
	    (guint64) stride * height > G_MAXSIZE ||
	    !isfinite (sx) || !isfinite (sy) || sx <= 0 || sy <= 0 ||
	    !isfinite (ox) || !isfinite (oy))
		goto invalid;

	mapping = map_buffer (handle, (guint64) stride * height, fd_list, TRUE, error);
	if (!mapping)
		return NULL;

	surface = cairo_image_surface_create_for_data (
	    (unsigned char *) g_mapped_file_get_contents (mapping),
	    format, width, height, stride);
	status = cairo_surface_status (surface);
	if (status == CAIRO_STATUS_SUCCESS) {
		status = cairo_surface_set_user_data (surface, &mapping_key, mapping,
		                                      (cairo_destroy_func_t) g_mapped_file_unref);
		if (status == CAIRO_STATUS_SUCCESS) {
			g_steal_pointer (&mapping);
			cairo_surface_set_device_scale (surface, sx, sy);
			cairo_surface_set_device_offset (surface, ox, oy);
			return surface;
		}
	}
	cairo_surface_destroy (surface);
	g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
	             "Cannot map rendered surface: %s", cairo_status_to_string (status));
	return NULL;

invalid:
	g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
	                     "Invalid rendered surface buffer");
	return NULL;
}

GVariant *
_pps_document_pixbuf_to_buffer (GdkPixbuf *pixbuf, GUnixFDList **fd_list, GError **error)
{
	if (!pixbuf || gdk_pixbuf_get_colorspace (pixbuf) != GDK_COLORSPACE_RGB || gdk_pixbuf_get_bits_per_sample (pixbuf) != 8) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Cannot transfer document image format");
		return NULL;
	}
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	int handle = append_data (gdk_pixbuf_read_pixels (pixbuf), gdk_pixbuf_get_byte_length (pixbuf), descriptors, error);
	if (handle == -1)
		return NULL;
	*fd_list = g_steal_pointer (&descriptors);
	return g_variant_new ("(hiiib)", handle, gdk_pixbuf_get_width (pixbuf), gdk_pixbuf_get_height (pixbuf),
	                      gdk_pixbuf_get_rowstride (pixbuf), gdk_pixbuf_get_has_alpha (pixbuf));
}

GdkPixbuf *
_pps_document_pixbuf_from_buffer (GVariant *buffer, GUnixFDList *fd_list, GError **error)
{
	g_autoptr (GBytes) bytes = NULL;
	int handle, width, height, stride;
	gboolean alpha;
	if (!buffer || !g_variant_is_of_type (buffer, PPS_DOCUMENT_PIXBUF_BUFFER_TYPE))
		goto invalid;
	g_variant_get (buffer, "(hiiib)", &handle, &width, &height, &stride, &alpha);
	int channels = alpha ? 4 : 3;
	if (width <= 0 || height <= 0 || width > G_MAXINT / channels || stride < width * channels)
		goto invalid;
	/* Pixbuf storage need not include the last row's padding. */
	guint64 size = (guint64) (height - 1) * stride + (guint64) width * channels;
	bytes = _pps_document_bytes_from_buffer (handle, size, fd_list, error);
	return bytes ? gdk_pixbuf_new_from_bytes (bytes, GDK_COLORSPACE_RGB, alpha, 8, width, height, stride) : NULL;
invalid:
	g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid document image buffer");
	return NULL;
}

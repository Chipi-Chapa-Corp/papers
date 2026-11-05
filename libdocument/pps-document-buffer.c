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

GVariant *
_pps_document_surface_to_buffer (cairo_surface_t *surface,
                                 GUnixFDList **fd_list,
                                 GError **error)
{
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	cairo_status_t status = surface ? cairo_surface_status (surface) : CAIRO_STATUS_NULL_POINTER;
	unsigned char *data;
	int fd, handle, width, height, stride;
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
	fd = memfd_create ("papers-render", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd == -1) {
		buffer_error (error);
		return NULL;
	}
	cairo_surface_flush (surface);
	data = cairo_image_surface_get_data (surface);
	for (gsize offset = 0; offset < size;) {
		ssize_t count = write (fd, data + offset, size - offset);
		if (count == -1 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (count == 0)
				errno = EIO;
			buffer_error (error);
			close (fd);
			return NULL;
		}
		offset += count;
	}
	if (fcntl (fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) == -1) {
		buffer_error (error);
		close (fd);
		return NULL;
	}
	handle = g_unix_fd_list_append (descriptors, fd, error);
	close (fd);
	if (handle == -1)
		return NULL;
	cairo_surface_get_device_scale (surface, &sx, &sy);
	cairo_surface_get_device_offset (surface, &ox, &oy);
	*fd_list = g_steal_pointer (&descriptors);
	return g_variant_new ("(hiiiidddd)", handle,
	                      cairo_image_surface_get_format (surface),
	                      width, height, stride, sx, sy, ox, oy);
}

cairo_surface_t *
_pps_document_surface_from_buffer (GVariant *buffer,
                                   GUnixFDList *fd_list,
                                   GError **error)
{
	g_autoptr (GMappedFile) mapping = NULL;
	int handle, format, width, height, stride, minimum_stride, fd, seals;
	double sx, sy, ox, oy;
	struct stat statbuf;
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

	fd = g_unix_fd_list_get (fd_list, handle, error);
	if (fd == -1)
		return NULL;
	seals = fcntl (fd, F_GET_SEALS);
	if (fstat (fd, &statbuf) == -1 || !S_ISREG (statbuf.st_mode) ||
	    statbuf.st_size < 0 || (guint64) statbuf.st_size != (guint64) stride * height ||
	    seals == -1 || (seals & (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE)) != (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE)) {
		close (fd);
		goto invalid;
	}
	mapping = g_mapped_file_new_from_fd (fd, TRUE, error);
	close (fd);
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

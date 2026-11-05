// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cairo.h>
#include <gio/gunixfdlist.h>

#include "pps-macros.h"

/* (handle, format, width, height, stride, scale-x, scale-y, offset-x, offset-y).
 * Pixels live in a sealed memfd; consumers map it privately so local drawing
 * cannot change the worker's result or another consumer's image.
 */
#define PPS_DOCUMENT_BUFFER_TYPE G_VARIANT_TYPE ("(hiiiidddd)")

PPS_PRIVATE
int _pps_document_bytes_to_buffer (GBytes *bytes, GUnixFDList *fd_list, GError **error);
PPS_PRIVATE
GBytes *_pps_document_bytes_from_buffer (int handle, guint64 size, GUnixFDList *fd_list, GError **error);
PPS_PRIVATE
GVariant *_pps_document_surface_append_buffer (cairo_surface_t *surface, GUnixFDList *fd_list, GError **error);

PPS_PRIVATE
GVariant *_pps_document_surface_to_buffer (cairo_surface_t *surface,
                                           GUnixFDList **fd_list,
                                           GError **error);
PPS_PRIVATE
cairo_surface_t *_pps_document_surface_from_buffer (GVariant *buffer,
                                                    GUnixFDList *fd_list,
                                                    GError **error);

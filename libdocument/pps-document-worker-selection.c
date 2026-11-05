// SPDX-License-Identifier: GPL-2.0-or-later

#include <math.h>

#include "pps-document-buffer-private.h"
#include "pps-document-geometry-private.h"
#include "pps-document-worker-private.h"

static gboolean
read_points (guint style, GVariant *value, PpsRectangle *points, GDBusMethodInvocation *invocation)
{
	if (style <= PPS_SELECTION_STYLE_LINE && _pps_rectangle_from_variant (value, points))
		return TRUE;
	g_dbus_method_invocation_return_error_literal (
	    invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid selection points or style");
	return FALSE;
}

static gboolean
read_color (GVariant *value, GdkRGBA *color, GDBusMethodInvocation *invocation)
{
	if (!_pps_rgba_from_variant (value, color)) {
		g_dbus_method_invocation_return_error_literal (
		    invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid selection color");
		return FALSE;
	}
	return TRUE;
}

static gboolean
get_selected_text (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, int index, guint style, GVariant *points, Worker *worker)
{
	g_autoptr (PpsPage) page = NULL;
	g_autofree char *text = NULL;
	PpsRectangle rectangle;
	if (!pps_document_worker_check_page (worker, index, PPS_TYPE_SELECTION, invocation) ||
	    !read_points (style, points, &rectangle, invocation))
		return TRUE;
	page = pps_document_get_page (worker->document, index);
	text = pps_selection_get_selected_text (PPS_SELECTION (worker->document), page, style, &rectangle);
	pps_dbus_document_complete_get_selected_text (interface, invocation, text != NULL, text ? text : "");
	return TRUE;
}

static gboolean
get_selection_region (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, int index, int rotation, double scale, int width, int height, guint style, GVariant *points, Worker *worker)
{
	g_autoptr (PpsRenderContext) context = pps_document_worker_create_render_context (
	    worker, index, PPS_TYPE_SELECTION, rotation, scale, width, height, PPS_RENDER_ANNOTS_ALL, invocation);
	PpsRectangle rectangle;
	cairo_region_t *region;
	if (!context || !read_points (style, points, &rectangle, invocation))
		return TRUE;
	region = pps_selection_get_selection_region (PPS_SELECTION (worker->document), context, style, &rectangle);
	pps_dbus_document_complete_get_selection_region (interface, invocation, region != NULL,
	                                                 _pps_region_to_variant (region));
	if (region)
		cairo_region_destroy (region);
	return TRUE;
}

static gboolean
render_selection (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, GUnixFDList *input_fds, int index, int rotation, double scale, int width, int height, guint annotations, guint style, GVariant *points, GVariant *text_color, GVariant *base_color, Worker *worker)
{
	g_autoptr (PpsRenderContext) context = pps_document_worker_create_render_context (
	    worker, index, PPS_TYPE_SELECTION, rotation, scale, width, height, annotations, invocation);
	g_autoptr (GUnixFDList) descriptors = NULL;
	g_autoptr (GError) error = NULL;
	PpsRectangle rectangle;
	GdkRGBA text, base;
	cairo_surface_t *surface = NULL;
	GVariant *buffer;
	if (!context || !read_points (style, points, &rectangle, invocation) ||
	    !read_color (text_color, &text, invocation) || !read_color (base_color, &base, invocation))
		return TRUE;
	/* A complete redraw avoids transferring the caller's previous surface. */
	pps_selection_render_selection (PPS_SELECTION (worker->document), context,
	                                &surface, &rectangle, NULL, style, &text, &base);
	if (!surface) {
		pps_dbus_document_complete_render_selection (
		    interface, invocation, NULL, FALSE,
		    g_variant_new ("(hiiiidddd)", 0, 0, 0, 0, 0, 1., 1., 0., 0.));
		return TRUE;
	}
	buffer = _pps_document_surface_to_buffer (surface, &descriptors, &error);
	cairo_surface_destroy (surface);
	if (!buffer)
		g_dbus_method_invocation_return_gerror (invocation, error);
	else
		pps_dbus_document_complete_render_selection (interface, invocation, descriptors, TRUE, buffer);
	return TRUE;
}

void
pps_document_worker_bind_selection (Worker *worker, PpsDBusDocument *interface)
{
	g_signal_connect (interface, "handle-get-selected-text", G_CALLBACK (get_selected_text), worker);
	g_signal_connect (interface, "handle-get-selection-region", G_CALLBACK (get_selection_region), worker);
	g_signal_connect (interface, "handle-render-selection", G_CALLBACK (render_selection), worker);
}

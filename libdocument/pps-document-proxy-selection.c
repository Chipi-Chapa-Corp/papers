// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-buffer-private.h"
#include "pps-document-geometry-private.h"
#include "pps-document-proxy-private.h"

static PpsDBusDocument *
get_proxy (PpsSelection *document)
{
	return _pps_document_proxy_get_session (PPS_DOCUMENT (document))->proxy;
}

static GVariant *
rectangle_value (PpsRectangle *points)
{
	return g_variant_new ("(dddd)", points->x1, points->y1, points->x2, points->y2);
}

static char *
get_selected_text (PpsSelection *document, PpsPage *page, PpsSelectionStyle style, PpsRectangle *points)
{
	g_autofree char *text = NULL;
	gboolean present;
	if (!pps_dbus_document_call_get_selected_text_sync (
		get_proxy (document), page->index, style, rectangle_value (points),
		G_DBUS_CALL_FLAGS_NONE, -1, &present, &text, NULL, NULL))
		return NULL;
	return present ? g_steal_pointer (&text) : NULL;
}

static cairo_region_t *
get_selection_region (PpsSelection *document, PpsRenderContext *context, PpsSelectionStyle style, PpsRectangle *points)
{
	g_autoptr (GVariant) region = NULL;
	g_autoptr (GError) error = NULL;
	gboolean present;
	if (!pps_dbus_document_call_get_selection_region_sync (
		get_proxy (document), context->page->index, context->rotation, context->scale,
		context->target_width, context->target_height, style, rectangle_value (points),
		G_DBUS_CALL_FLAGS_NONE, -1, &present, &region, NULL, NULL) ||
	    !present)
		return NULL;
	return _pps_region_from_variant (region, &error);
}

static void
render_selection (PpsSelection *document, PpsRenderContext *context, cairo_surface_t **surface, PpsRectangle *points, PpsRectangle *old_points, PpsSelectionStyle style, GdkRGBA *text, GdkRGBA *base)
{
	g_autoptr (GVariant) buffer = NULL;
	g_autoptr (GUnixFDList) descriptors = NULL;
	g_autoptr (GError) error = NULL;
	gboolean present;
	cairo_surface_t *rendered;
	if (!pps_dbus_document_call_render_selection_sync (
		get_proxy (document), context->page->index, context->rotation, context->scale,
		context->target_width, context->target_height, context->annot_flags, style,
		rectangle_value (points),
		g_variant_new ("(dddd)", (double) text->red, (double) text->green, (double) text->blue, (double) text->alpha),
		g_variant_new ("(dddd)", (double) base->red, (double) base->green, (double) base->blue, (double) base->alpha),
		G_DBUS_CALL_FLAGS_NONE, -1, NULL, &present, &buffer, &descriptors, NULL, &error)) {
		g_debug ("Selection rendering failed: %s", error->message);
		return;
	}
	if (!present)
		return;
	rendered = _pps_document_surface_from_buffer (buffer, descriptors, &error);
	if (!rendered) {
		g_debug ("Selection buffer failed: %s", error->message);
		return;
	}
	if (*surface) {
		double sx, sy, ox, oy;
		cairo_surface_get_device_scale (rendered, &sx, &sy);
		cairo_surface_get_device_offset (rendered, &ox, &oy);
		cairo_surface_set_device_scale (*surface, sx, sy);
		cairo_surface_set_device_offset (*surface, ox, oy);
		cairo_t *cr = cairo_create (*surface);
		cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
		cairo_set_source_surface (cr, rendered, 0, 0);
		cairo_paint (cr);
		cairo_destroy (cr);
		cairo_surface_destroy (rendered);
	} else {
		*surface = rendered;
	}
}

void
_pps_document_proxy_selection_iface_init (PpsSelectionInterface *iface)
{
	iface->get_selected_text = get_selected_text;
	iface->get_selection_region = get_selection_region;
	iface->render_selection = render_selection;
}

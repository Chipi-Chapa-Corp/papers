// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-geometry-private.h"
#include "pps-document-worker-private.h"

static gboolean
get_text (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, int index, Worker *worker)
{
	g_autoptr (PpsPage) page = NULL;
	g_autofree char *text = NULL;
	if (!pps_document_worker_check_page (worker, index, PPS_TYPE_DOCUMENT_TEXT, invocation))
		return TRUE;
	page = pps_document_get_page (worker->document, index);
	text = pps_document_text_get_text (PPS_DOCUMENT_TEXT (worker->document), page);
	pps_dbus_document_complete_get_text (interface, invocation, text != NULL, text ? text : "");
	return TRUE;
}

static gboolean
get_text_layout (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, int index, Worker *worker)
{
	g_autoptr (PpsPage) page = NULL;
	g_autofree PpsRectangle *areas = NULL;
	guint n_areas = 0;
	gboolean present;
	if (!pps_document_worker_check_page (worker, index, PPS_TYPE_DOCUMENT_TEXT, invocation))
		return TRUE;
	page = pps_document_get_page (worker->document, index);
	present = pps_document_text_get_text_layout (PPS_DOCUMENT_TEXT (worker->document),
	                                             page, &areas, &n_areas);
	pps_dbus_document_complete_get_text_layout (interface, invocation, present,
	                                            _pps_rectangles_to_variant (areas, n_areas));
	return TRUE;
}

static gboolean
get_text_mapping (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, int index, Worker *worker)
{
	g_autoptr (PpsPage) page = NULL;
	cairo_region_t *region;
	if (!pps_document_worker_check_page (worker, index, PPS_TYPE_DOCUMENT_TEXT, invocation))
		return TRUE;
	page = pps_document_get_page (worker->document, index);
	region = pps_document_text_get_text_mapping (PPS_DOCUMENT_TEXT (worker->document), page);
	pps_dbus_document_complete_get_text_mapping (interface, invocation, region != NULL,
	                                             _pps_region_to_variant (region));
	if (region)
		cairo_region_destroy (region);
	return TRUE;
}

static gboolean
get_text_attrs (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, int index, Worker *worker)
{
	g_autoptr (PpsPage) page = NULL;
	g_autoptr (PangoAttrList) attrs = NULL;
	g_autofree char *text = NULL;
	if (!pps_document_worker_check_page (worker, index, PPS_TYPE_DOCUMENT_TEXT, invocation))
		return TRUE;
	page = pps_document_get_page (worker->document, index);
	attrs = pps_document_text_get_text_attrs (PPS_DOCUMENT_TEXT (worker->document), page);
	if (attrs)
		text = pango_attr_list_to_string (attrs);
	pps_dbus_document_complete_get_text_attrs (interface, invocation, attrs != NULL, text ? text : "");
	return TRUE;
}

static gboolean
get_text_in_area (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, int index, GVariant *area, Worker *worker)
{
	g_autoptr (PpsPage) page = NULL;
	g_autofree char *text = NULL;
	PpsRectangle rectangle;
	if (!pps_document_worker_check_page (worker, index, PPS_TYPE_DOCUMENT_TEXT, invocation))
		return TRUE;
	if (!_pps_rectangle_from_variant (area, &rectangle)) {
		g_dbus_method_invocation_return_error_literal (
		    invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid text area");
		return TRUE;
	}
	page = pps_document_get_page (worker->document, index);
	text = pps_document_text_get_text_in_area (PPS_DOCUMENT_TEXT (worker->document), page, &rectangle);
	pps_dbus_document_complete_get_text_in_area (interface, invocation, text != NULL, text ? text : "");
	return TRUE;
}

static gboolean
find_text (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, int index, const char *text, guint options, Worker *worker)
{
	g_autoptr (PpsPage) page = NULL;
	GVariantBuilder builder;
	GList *matches;
	if (!pps_document_worker_check_page (worker, index, PPS_TYPE_DOCUMENT_FIND, invocation))
		return TRUE;
	page = pps_document_get_page (worker->document, index);
	matches = pps_document_find_find_text (PPS_DOCUMENT_FIND (worker->document), page, text, options);
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ddddbb)"));
	for (GList *item = matches; item; item = item->next) {
		PpsFindRectangle *rectangle = item->data;
		g_variant_builder_add (&builder, "(ddddbb)", rectangle->x1, rectangle->y1,
		                       rectangle->x2, rectangle->y2, rectangle->next_line, rectangle->after_hyphen);
	}
	g_list_free_full (matches, (GDestroyNotify) pps_find_rectangle_free);
	pps_dbus_document_complete_find_text (interface, invocation, g_variant_builder_end (&builder));
	return TRUE;
}

void
pps_document_worker_bind_text (Worker *worker, PpsDBusDocument *interface)
{
	g_signal_connect (interface, "handle-get-text", G_CALLBACK (get_text), worker);
	g_signal_connect (interface, "handle-get-text-layout", G_CALLBACK (get_text_layout), worker);
	g_signal_connect (interface, "handle-get-text-mapping", G_CALLBACK (get_text_mapping), worker);
	g_signal_connect (interface, "handle-get-text-attrs", G_CALLBACK (get_text_attrs), worker);
	g_signal_connect (interface, "handle-get-text-in-area", G_CALLBACK (get_text_in_area), worker);
	g_signal_connect (interface, "handle-find-text", G_CALLBACK (find_text), worker);
}

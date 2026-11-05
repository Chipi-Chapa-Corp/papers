// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-variant-private.h"
#include "pps-document-worker-private.h"

static gboolean
scan_fonts (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, Worker *worker)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_FONTS, invocation))
		return TRUE;
	pps_document_fonts_scan (PPS_DOCUMENT_FONTS (worker->document));
	pps_dbus_document_complete_scan_fonts (interface, invocation);
	return TRUE;
}

static gboolean
get_fonts (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, Worker *worker)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_FONTS, invocation))
		return TRUE;
	g_autoptr (GListModel) model = pps_document_fonts_get_model (PPS_DOCUMENT_FONTS (worker->document));
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));
	for (guint i = 0; model && i < g_list_model_get_n_items (model); i++) {
		g_autoptr (GObject) font = g_list_model_get_item (model, i);
		g_autofree char *name = NULL, *details = NULL;
		g_object_get (font, "name", &name, "details", &details, NULL);
		GVariantBuilder properties;
		g_variant_builder_init (&properties, G_VARIANT_TYPE_VARDICT);
		_pps_variant_add_string (&properties, "name", name);
		_pps_variant_add_string (&properties, "details", details);
		g_variant_builder_add_value (&builder, g_variant_builder_end (&properties));
	}
	pps_dbus_document_complete_get_fonts (interface, invocation, model != NULL, g_variant_builder_end (&builder));
	return TRUE;
}

static gboolean
get_summary (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, Worker *worker)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_FONTS, invocation))
		return TRUE;
	const char *summary = pps_document_fonts_get_fonts_summary (PPS_DOCUMENT_FONTS (worker->document));
	pps_dbus_document_complete_get_fonts_summary (interface, invocation, summary != NULL, summary ? summary : "");
	return TRUE;
}

void
pps_document_worker_bind_fonts (Worker *worker, PpsDBusDocument *interface)
{
	g_signal_connect (interface, "handle-scan-fonts", G_CALLBACK (scan_fonts), worker);
	g_signal_connect (interface, "handle-get-fonts", G_CALLBACK (get_fonts), worker);
	g_signal_connect (interface, "handle-get-fonts-summary", G_CALLBACK (get_summary), worker);
}

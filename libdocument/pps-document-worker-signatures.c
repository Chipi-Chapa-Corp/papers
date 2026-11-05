// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-signature-private.h"
#include "pps-document-signatures.h"
#include "pps-document-worker-private.h"

static gboolean
get_signatures (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, Worker *worker)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_SIGNATURES, invocation))
		return TRUE;
	g_autolist (PpsSignature) signatures = pps_document_signatures_get_signatures (PPS_DOCUMENT_SIGNATURES (worker->document));
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));
	for (GList *item = signatures; item; item = item->next)
		g_variant_builder_add_value (&builder, _pps_signature_info_to_variant (item->data));
	pps_dbus_document_complete_get_signatures (interface, invocation, g_variant_builder_end (&builder));
	return TRUE;
}

void
pps_document_worker_bind_signatures (Worker *worker, PpsDBusDocument *interface)
{
	g_signal_connect (interface, "handle-get-signatures", G_CALLBACK (get_signatures), worker);
}

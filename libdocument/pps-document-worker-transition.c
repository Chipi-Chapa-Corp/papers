// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-transition-private.h"
#include "pps-document-worker-private.h"

static gboolean
get_duration (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, int page, Worker *worker)
{
	if (!pps_document_worker_check_page (worker, page, PPS_TYPE_DOCUMENT_TRANSITION, invocation))
		return TRUE;
	pps_dbus_document_complete_get_page_duration (interface, invocation,
	                                              pps_document_transition_get_page_duration (PPS_DOCUMENT_TRANSITION (worker->document), page));
	return TRUE;
}

static gboolean
get_effect (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, int page, Worker *worker)
{
	if (!pps_document_worker_check_page (worker, page, PPS_TYPE_DOCUMENT_TRANSITION, invocation))
		return TRUE;
	g_autoptr (PpsTransitionEffect) effect = pps_document_transition_get_effect (PPS_DOCUMENT_TRANSITION (worker->document), page);
	pps_dbus_document_complete_get_transition_effect (interface, invocation, _pps_transition_effect_to_variant (effect));
	return TRUE;
}

void
pps_document_worker_bind_transition (Worker *worker, PpsDBusDocument *interface)
{
	g_signal_connect (interface, "handle-get-page-duration", G_CALLBACK (get_duration), worker);
	g_signal_connect (interface, "handle-get-transition-effect", G_CALLBACK (get_effect), worker);
}

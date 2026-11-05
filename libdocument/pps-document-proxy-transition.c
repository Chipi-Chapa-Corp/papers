// SPDX-License-Identifier: GPL-2.0-or-later

#include <math.h>

#include "pps-document-proxy-private.h"
#include "pps-document-transition-private.h"

static double
get_duration (PpsDocumentTransition *document, int page)
{
	g_autoptr (PpsDocumentTransition) hold = g_object_ref (document);
	double duration = -1;
	if (!pps_dbus_document_call_get_page_duration_sync (
		_pps_document_proxy_get_session (PPS_DOCUMENT (document))->proxy, page,
		G_DBUS_CALL_FLAGS_NONE, -1, &duration, NULL, NULL) ||
	    !isfinite (duration))
		return -1;
	return duration;
}

static PpsTransitionEffect *
get_effect (PpsDocumentTransition *document, int page)
{
	g_autoptr (PpsDocumentTransition) hold = g_object_ref (document);
	g_autoptr (GVariant) value = NULL;
	g_autoptr (GError) error = NULL;
	if (!pps_dbus_document_call_get_transition_effect_sync (
		_pps_document_proxy_get_session (PPS_DOCUMENT (document))->proxy, page,
		G_DBUS_CALL_FLAGS_NONE, -1, &value, NULL, &error))
		return NULL;
	return _pps_transition_effect_from_variant (value, &error);
}

void
_pps_document_proxy_transition_iface_init (PpsDocumentTransitionInterface *iface)
{
	iface->get_page_duration = get_duration;
	iface->get_effect = get_effect;
}

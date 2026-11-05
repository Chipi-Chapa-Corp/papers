// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-document-session-private.h"
#include "pps-document.h"

#define PPS_TYPE_DOCUMENT_PROXY (pps_document_proxy_get_type ())
PPS_PRIVATE
G_DECLARE_DERIVABLE_TYPE (PpsDocumentProxy, pps_document_proxy, PPS, DOCUMENT_PROXY, PpsDocument)

struct _PpsDocumentProxyClass {
	PpsDocumentClass parent_class;
};

/* Kept private until all backend capabilities have proxy implementations. */
PPS_PRIVATE
PpsDocument *_pps_document_proxy_new (const char *mime_type,
                                      GCancellable *cancellable,
                                      GError **error);
PPS_PRIVATE
PpsDocumentSession *_pps_document_proxy_get_session (PpsDocument *document);

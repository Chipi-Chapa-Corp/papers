// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-document-find.h"
#include "pps-document-session-private.h"
#include "pps-document-text.h"
#include "pps-document.h"
#include "pps-selection.h"

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
PPS_PRIVATE
guint _pps_document_proxy_get_find_options (PpsDocument *document);

void _pps_document_proxy_text_iface_init (PpsDocumentTextInterface *iface);
void _pps_document_proxy_find_iface_init (PpsDocumentFindInterface *iface);
void _pps_document_proxy_selection_iface_init (PpsSelectionInterface *iface);

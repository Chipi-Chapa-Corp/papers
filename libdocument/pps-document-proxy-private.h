// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-document-annotations.h"
#include "pps-document-attachments.h"
#include "pps-document-find.h"
#include "pps-document-fonts.h"
#include "pps-document-forms.h"
#include "pps-document-images.h"
#include "pps-document-layers.h"
#include "pps-document-links.h"
#include "pps-document-objects-private.h"
#include "pps-document-session-private.h"
#include "pps-document-state-private.h"
#include "pps-document-text.h"
#include "pps-document-transition.h"
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
GType _pps_document_proxy_type_for_interfaces (const char *const *interfaces, guint annotation_methods);
PPS_PRIVATE
PpsDocumentSession *_pps_document_proxy_get_session (PpsDocument *document);
PPS_PRIVATE
GVariant *_pps_document_proxy_get_metadata (PpsDocument *document);
PPS_PRIVATE
PpsDocumentObjects *_pps_document_proxy_get_objects (PpsDocument *document);
PPS_PRIVATE
guint _pps_document_proxy_get_state (PpsDocument *document);
PPS_PRIVATE
void _pps_document_proxy_set_state (PpsDocument *document, guint state);

void _pps_document_proxy_text_iface_init (PpsDocumentTextInterface *iface);
void _pps_document_proxy_find_iface_init (PpsDocumentFindInterface *iface);
void _pps_document_proxy_selection_iface_init (PpsSelectionInterface *iface);
void _pps_document_proxy_links_iface_init (PpsDocumentLinksInterface *iface);
void _pps_document_proxy_layers_iface_init (PpsDocumentLayersInterface *iface);
void _pps_document_proxy_forms_iface_init (PpsDocumentFormsInterface *iface);
void _pps_document_proxy_attachments_iface_init (PpsDocumentAttachmentsInterface *iface);
void _pps_document_proxy_annotations_iface_init (PpsDocumentAnnotationsInterface *iface, gpointer data);
void _pps_document_proxy_fonts_iface_init (PpsDocumentFontsInterface *iface);
void _pps_document_proxy_images_iface_init (PpsDocumentImagesInterface *iface);
void _pps_document_proxy_transition_iface_init (PpsDocumentTransitionInterface *iface);

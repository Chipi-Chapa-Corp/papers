// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-document-objects-private.h"
#include <papers-document.h>

PPS_PRIVATE
GVariant *_pps_link_dest_to_variant (PpsLinkDest *dest);
PPS_PRIVATE
PpsLinkDest *_pps_link_dest_from_variant (GVariant *value, GError **error);
PPS_PRIVATE
GVariant *_pps_layer_to_variant (PpsLayer *layer, PpsDocumentObjects *objects);
PPS_PRIVATE
PpsLayer *_pps_layer_from_variant (GVariant *value, PpsDocumentObjects *objects, GError **error);
PPS_PRIVATE
GVariant *_pps_link_to_variant (PpsLink *link, PpsDocumentObjects *objects);
PPS_PRIVATE
PpsLink *_pps_link_from_variant (GVariant *value, PpsDocumentObjects *objects, GError **error);
PPS_PRIVATE
GVariant *_pps_outlines_to_variant (GListModel *model, PpsDocumentObjects *objects, GError **error);
PPS_PRIVATE
GListModel *_pps_outlines_from_variant (GVariant *value, PpsDocumentObjects *objects, GError **error);
PPS_PRIVATE
GListModel *_pps_layers_from_variant (GVariant *value, PpsDocumentObjects *objects, GError **error);

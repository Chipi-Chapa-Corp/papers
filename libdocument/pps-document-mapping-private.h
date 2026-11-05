// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-document-variant-private.h"
#include "pps-mapping-list.h"

PPS_PRIVATE
GVariant *_pps_mapping_list_to_variant (PpsMappingList *mappings, PpsDocumentEncodeItem encode, gpointer data);
PPS_PRIVATE
PpsMappingList *_pps_mapping_list_from_variant (GVariant *value, guint page, PpsDocumentDecodeItem decode, gpointer data, GError **error);

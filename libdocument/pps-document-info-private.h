// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-document-info.h"

PPS_PRIVATE
GVariant *_pps_document_info_to_variant (const PpsDocumentInfo *info);
PPS_PRIVATE
PpsDocumentInfo *_pps_document_info_from_variant (GVariant *value, GError **error);

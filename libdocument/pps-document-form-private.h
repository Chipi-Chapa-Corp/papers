// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-document-objects-private.h"
#include "pps-form-field-private.h"

G_DEFINE_AUTOPTR_CLEANUP_FUNC (PpsFormField, g_object_unref)

PPS_PRIVATE
GVariant *_pps_form_field_to_variant (PpsFormField *field, PpsDocumentObjects *objects);
PPS_PRIVATE
PpsFormField *_pps_form_field_from_variant (GVariant *value, PpsDocumentObjects *objects, GError **error);

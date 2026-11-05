// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-document-variant-private.h"
#include "pps-macros.h"
#include <gio/gio.h>

typedef void (*PpsDocumentSetChildren) (GObject *item, GListModel *children);

/* Rows contain a preceding parent index (-1 for roots), a children-present flag,
 * and the item's properties. The wire format has constant nesting depth.
 */
PPS_PRIVATE
GVariant *_pps_document_tree_to_variant (GListModel *model, PpsDocumentEncodeItem encode, gpointer data, GError **error);
PPS_PRIVATE
GListModel *_pps_document_tree_from_variant (GVariant *value, GType item_type, PpsDocumentDecodeItem decode, PpsDocumentSetChildren set_children, gpointer data, GError **error);

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-macros.h"
#include <gio/gio.h>

/* Handles belong to one document session and are never reused. */
typedef struct _PpsDocumentObjects PpsDocumentObjects;

PPS_PRIVATE
PpsDocumentObjects *_pps_document_objects_new (void);
PPS_PRIVATE
void _pps_document_objects_free (PpsDocumentObjects *objects);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PpsDocumentObjects, _pps_document_objects_free)
PPS_PRIVATE
guint64 _pps_document_objects_export (PpsDocumentObjects *objects, GObject *object);
PPS_PRIVATE
guint64 _pps_document_objects_get_id (PpsDocumentObjects *objects, GObject *object);
PPS_PRIVATE
GObject *_pps_document_objects_lookup (PpsDocumentObjects *objects, guint64 id, GType type);
PPS_PRIVATE
gboolean _pps_document_objects_import (PpsDocumentObjects *objects, guint64 id, GObject *object);
/* Bind an independent local wrapper without retaining it. */
PPS_PRIVATE
gboolean _pps_document_objects_bind (PpsDocumentObjects *objects, guint64 id, GObject *object);

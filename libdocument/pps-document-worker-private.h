// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <papers-document.h>

#include "pps-document-objects-private.h"
#include "pps-document-session-private.h"
#include "pps-document-state-private.h"

typedef struct {
	PpsDocument *document;
	gboolean loaded;
	int export_fd;
	PpsDocumentObjects *objects;
	GListModel *outlines;
	GListModel *layers;
	GHashTable *links;
	GHashTable *forms;
	GPtrArray *attachments;
	GHashTable *annotations;
	GHashTable *images;
	GHashTable *media;
	gboolean outlines_loaded;
	gboolean layers_loaded;
} Worker;

gboolean pps_document_worker_check_page (Worker *worker, int page, GType capability, GDBusMethodInvocation *invocation);
void pps_document_worker_bind_text (Worker *worker, PpsDBusDocument *interface);
void pps_document_worker_bind_selection (Worker *worker, PpsDBusDocument *interface);
void pps_document_worker_bind_links (Worker *worker, PpsDBusDocument *interface);
void pps_document_worker_bind_forms (Worker *worker, PpsDBusDocument *interface);
void pps_document_worker_bind_attachments (Worker *worker, PpsDBusDocument *interface);
void pps_document_worker_bind_annotations (Worker *worker, PpsDBusDocument *interface);
void pps_document_worker_bind_fonts (Worker *worker, PpsDBusDocument *interface);
void pps_document_worker_bind_images (Worker *worker, PpsDBusDocument *interface);
void pps_document_worker_bind_transition (Worker *worker, PpsDBusDocument *interface);
void pps_document_worker_bind_media (Worker *worker, PpsDBusDocument *interface);
void pps_document_worker_bind_exporter (Worker *worker, PpsDBusDocument *interface);
void pps_document_worker_bind_signatures (Worker *worker, PpsDBusDocument *interface);
void pps_document_worker_clear_exporter (Worker *worker);
void pps_document_worker_clear_links (Worker *worker);
gboolean pps_document_worker_check_document (Worker *worker, GType capability, GDBusMethodInvocation *invocation);
guint pps_document_worker_get_state (Worker *worker);
PpsRenderContext *pps_document_worker_create_render_context (
    Worker *worker, int page, GType capability, int rotation, double scale, int target_width, int target_height, guint annotations, GDBusMethodInvocation *invocation);

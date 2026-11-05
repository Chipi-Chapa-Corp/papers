// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <papers-document.h>

#include "pps-document-session-private.h"

typedef struct {
	PpsDocument *document;
	gboolean loaded;
} Worker;

gboolean pps_document_worker_check_page (Worker *worker, int page, GType capability, GDBusMethodInvocation *invocation);
void pps_document_worker_bind_text (Worker *worker, PpsDBusDocument *interface);
void pps_document_worker_bind_selection (Worker *worker, PpsDBusDocument *interface);
PpsRenderContext *pps_document_worker_create_render_context (
    Worker *worker, int page, GType capability, int rotation, double scale, int target_width, int target_height, guint annotations, GDBusMethodInvocation *invocation);

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

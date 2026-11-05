// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-document-dbus.h"

#define PPS_DOCUMENT_WORKER_PATH "/org/gnome/Papers/Document"
#define PPS_DOCUMENT_WORKER_FD 3

typedef struct {
	GSubprocess *process;
	GDBusConnection *connection;
	PpsDBusDocument *proxy;
} PpsDocumentSession;

PPS_PRIVATE
void _pps_document_session_free (PpsDocumentSession *session);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (PpsDocumentSession, _pps_document_session_free)

PPS_PRIVATE
PpsDocumentSession *_pps_document_session_new (GCancellable *cancellable,
                                               GError **error);

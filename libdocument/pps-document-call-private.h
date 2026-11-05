// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-macros.h"
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#define PPS_DOCUMENT_PASSWORD_PENDING "pps-document-password-pending"

PPS_PRIVATE
GVariant *_pps_document_call_sync (GDBusProxy *proxy, const char *method, GVariant *parameters, GDBusCallFlags flags, int timeout, GCancellable *cancellable, GError **error);
PPS_PRIVATE
GVariant *_pps_document_call_with_fds_sync (GDBusProxy *proxy, const char *method, GVariant *parameters, GDBusCallFlags flags, int timeout, GUnixFDList *fds, GUnixFDList **out_fds, GCancellable *cancellable, GError **error);

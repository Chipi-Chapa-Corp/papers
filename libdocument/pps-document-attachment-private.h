// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-attachment-private.h"
#include "pps-document-buffer-private.h"

PPS_PRIVATE
GVariant *_pps_attachment_to_variant (PpsAttachment *attachment, GUnixFDList *fd_list, GError **error);
PPS_PRIVATE
PpsAttachment *_pps_attachment_from_variant (GVariant *value, GUnixFDList *fd_list, GError **error);

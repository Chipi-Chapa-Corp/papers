// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-document.h"

/* Instantiate the backend directly, for use inside the document worker. */
PPS_PRIVATE
PpsDocument *_pps_document_factory_create_document (const char *mime_type,
                                                    GError **error);

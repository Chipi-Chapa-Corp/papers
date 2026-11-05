// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-attachment.h"

/* The returned immutable bytes retain the attachment's payload. */
PPS_PRIVATE
GBytes *_pps_attachment_get_data (PpsAttachment *attachment);

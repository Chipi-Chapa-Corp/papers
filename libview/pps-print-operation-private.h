// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-print-operation.h"

/* Keep rendering on the worker's dispatch context, serial with other requests. */
PPS_PRIVATE
PpsPrintOperation *_pps_print_operation_new_for_worker (PpsDocument *document);

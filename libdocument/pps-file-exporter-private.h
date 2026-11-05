// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-file-exporter.h"

/* Retain the first failure until the next export; NULL starts a new operation. */
PPS_PRIVATE
void _pps_file_exporter_take_error (PpsFileExporter *exporter, GError *error);
PPS_PRIVATE
gboolean _pps_file_exporter_get_error (PpsFileExporter *exporter, GError **error);

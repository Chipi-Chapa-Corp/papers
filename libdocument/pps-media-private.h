// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-media.h"
#include <gio/gio.h>

/* The temporary file is removed after its last shared reference is released. */
PPS_PRIVATE
void _pps_media_set_temp_file (PpsMedia *media, GFile *file);
PPS_PRIVATE
GFile *_pps_media_get_temp_file (PpsMedia *media);

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-macros.h"
#include <glib.h>

PPS_PRIVATE
void _pps_variant_add_string (GVariantBuilder *builder, const char *name, const char *value);
PPS_PRIVATE
gboolean _pps_variant_read_string (GVariant *dictionary, const char *name, char **out);

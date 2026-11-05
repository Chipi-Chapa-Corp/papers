// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-document.h"

PPS_PRIVATE
GVariant *_pps_rectangles_to_variant (const PpsRectangle *areas, guint n_areas);
PPS_PRIVATE
gboolean _pps_rectangles_from_variant (GVariant *value, PpsRectangle **areas, guint *n_areas, GError **error);
PPS_PRIVATE
GVariant *_pps_region_to_variant (cairo_region_t *region);
PPS_PRIVATE
cairo_region_t *_pps_region_from_variant (GVariant *value, GError **error);
PPS_PRIVATE
gboolean _pps_rectangle_from_variant (GVariant *value, PpsRectangle *area);

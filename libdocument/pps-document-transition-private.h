// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-transition-effect.h"
#include <gio/gio.h>

/* type, alignment, direction, legacy duration, duration, angle, scale, rectangular */
#define PPS_TRANSITION_EFFECT_VARIANT_TYPE G_VARIANT_TYPE ("(iiiididb)")

PPS_PRIVATE
GVariant *_pps_transition_effect_to_variant (PpsTransitionEffect *effect);
PPS_PRIVATE
PpsTransitionEffect *_pps_transition_effect_from_variant (GVariant *value, GError **error);

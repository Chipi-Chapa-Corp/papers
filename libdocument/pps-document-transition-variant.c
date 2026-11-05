// SPDX-License-Identifier: GPL-2.0-or-later

#include <math.h>

#include "pps-document-transition-private.h"

GVariant *
_pps_transition_effect_to_variant (PpsTransitionEffect *effect)
{
	int type, alignment, direction, legacy_duration, angle;
	double duration, scale;
	gboolean rectangular;
	g_object_get (effect, "type", &type, "alignment", &alignment, "direction", &direction,
	              "duration", &legacy_duration, "duration-real", &duration, "angle", &angle,
	              "scale", &scale, "rectangular", &rectangular, NULL);
	return g_variant_new ("(iiiididb)", type, alignment, direction, legacy_duration, duration, angle, scale, rectangular);
}

PpsTransitionEffect *
_pps_transition_effect_from_variant (GVariant *value, GError **error)
{
	int type, alignment, direction, legacy_duration, angle;
	double duration, scale;
	gboolean rectangular;
	if (!value || !g_variant_is_of_type (value, PPS_TRANSITION_EFFECT_VARIANT_TYPE))
		goto invalid;
	g_variant_get (value, "(iiiididb)", &type, &alignment, &direction, &legacy_duration, &duration, &angle, &scale, &rectangular);
	if (type < PPS_TRANSITION_EFFECT_REPLACE || type > PPS_TRANSITION_EFFECT_FADE ||
	    alignment < PPS_TRANSITION_ALIGNMENT_HORIZONTAL || alignment > PPS_TRANSITION_ALIGNMENT_VERTICAL ||
	    direction < PPS_TRANSITION_DIRECTION_INWARD || direction > PPS_TRANSITION_DIRECTION_OUTWARD ||
	    legacy_duration < 0 || !isfinite (duration) || duration < 0 || duration > 86400 ||
	    angle < 0 || angle > 360 || !isfinite (scale) || scale < 0 || scale > 1)
		goto invalid;
	return pps_transition_effect_new (type, "alignment", alignment, "direction", direction,
	                                  "duration", legacy_duration, "duration-real", duration, "angle", angle,
	                                  "scale", scale, "rectangular", rectangular, NULL);
invalid:
	g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid document transition effect");
	return NULL;
}

// SPDX-License-Identifier: GPL-2.0-or-later

#include <math.h>

#include "pps-document-transition-private.h"

static void
roundtrip (void)
{
	const char *properties[] = { "type", "alignment", "direction", "duration", "duration-real", "angle", "scale", "rectangular" };
	for (int type = PPS_TRANSITION_EFFECT_REPLACE; type <= PPS_TRANSITION_EFFECT_FADE; type++) {
		g_autoptr (PpsTransitionEffect) effect = pps_transition_effect_new (type,
		                                                                    "alignment", PPS_TRANSITION_ALIGNMENT_VERTICAL,
		                                                                    "direction", PPS_TRANSITION_DIRECTION_OUTWARD,
		                                                                    "duration", 2, "duration-real", 2.75,
		                                                                    "angle", 270, "scale", .625, "rectangular", TRUE, NULL);
		g_autoptr (GVariant) value = g_variant_ref_sink (_pps_transition_effect_to_variant (effect));
		g_autoptr (GError) error = NULL;
		g_autoptr (PpsTransitionEffect) copy = _pps_transition_effect_from_variant (value, &error);
		g_assert_no_error (error);
		g_assert_nonnull (copy);
		for (guint i = 0; i < G_N_ELEMENTS (properties); i++) {
			GParamSpec *spec = g_object_class_find_property (G_OBJECT_GET_CLASS (effect), properties[i]);
			GValue original = G_VALUE_INIT, result = G_VALUE_INIT;
			g_value_init (&original, spec->value_type);
			g_value_init (&result, spec->value_type);
			g_object_get_property (G_OBJECT (effect), properties[i], &original);
			g_object_get_property (G_OBJECT (copy), properties[i], &result);
			g_assert_cmpint (g_param_values_cmp (spec, &original, &result), ==, 0);
			g_value_unset (&original);
			g_value_unset (&result);
		}
	}
}

static void
boundaries (void)
{
	g_autoptr (GError) error = NULL;
	g_autoptr (GVariant) value = g_variant_ref_sink (g_variant_new ("(iiiididb)", 0, 0, 0, G_MAXINT, 86400., 360, 0., FALSE));
	g_autoptr (PpsTransitionEffect) effect = _pps_transition_effect_from_variant (value, &error);
	g_assert_no_error (error);
	g_assert_nonnull (effect);
	g_autoptr (GVariant) copy = g_variant_ref_sink (_pps_transition_effect_to_variant (effect));
	g_assert_true (g_variant_equal (value, copy));
}

static void
invalid_effects (void)
{
	const struct {
		int type, alignment, direction, legacy_duration;
		double duration;
		int angle;
		double scale;
	} cases[] = {
		{ -1, 0, 0, 0, 1, 0, 1 },
		{ PPS_TRANSITION_EFFECT_FADE + 1, 0, 0, 0, 1, 0, 1 },
		{ 0, -1, 0, 0, 1, 0, 1 },
		{ 0, 2, 0, 0, 1, 0, 1 },
		{ 0, 0, -1, 0, 1, 0, 1 },
		{ 0, 0, 2, 0, 1, 0, 1 },
		{ 0, 0, 0, -1, 1, 0, 1 },
		{ 0, 0, 0, 0, -1, 0, 1 },
		{ 0, 0, 0, 0, 86401, 0, 1 },
		{ 0, 0, 0, 0, NAN, 0, 1 },
		{ 0, 0, 0, 0, 1, -1, 1 },
		{ 0, 0, 0, 0, 1, 361, 1 },
		{ 0, 0, 0, 0, 1, 0, -1 },
		{ 0, 0, 0, 0, 1, 0, 1.01 },
		{ 0, 0, 0, 0, 1, 0, INFINITY },
	};
	for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
		g_autoptr (GError) error = NULL;
		g_autoptr (GVariant) value = g_variant_ref_sink (g_variant_new ("(iiiididb)", cases[i].type, cases[i].alignment, cases[i].direction,
		                                                                cases[i].legacy_duration, cases[i].duration, cases[i].angle, cases[i].scale, FALSE));
		g_assert_null (_pps_transition_effect_from_variant (value, &error));
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	}
	g_autoptr (GError) error = NULL;
	g_autoptr (GVariant) wrong_type = g_variant_ref_sink (g_variant_new_int32 (0));
	g_assert_null (_pps_transition_effect_from_variant (wrong_type, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_clear_error (&error);
	g_assert_null (_pps_transition_effect_from_variant (NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/document-transition/roundtrip", roundtrip);
	g_test_add_func ("/document-transition/boundaries", boundaries);
	g_test_add_func ("/document-transition/invalid-effects", invalid_effects);
	return g_test_run ();
}

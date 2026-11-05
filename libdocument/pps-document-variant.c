// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-variant-private.h"

void
_pps_variant_add_string (GVariantBuilder *builder, const char *name, const char *value)
{
	if (value)
		g_variant_builder_add (builder, "{sv}", name, g_variant_new_string (value));
}

gboolean
_pps_variant_read_string (GVariant *dictionary, const char *name, char **out)
{
	g_autoptr (GVariant) value = g_variant_lookup_value (dictionary, name, NULL);
	if (!value)
		return TRUE;
	if (!g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
		return FALSE;
	*out = g_variant_dup_string (value, NULL);
	return TRUE;
}

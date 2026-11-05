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

void
_pps_variant_add_date (GVariantBuilder *builder, const char *name, GDateTime *date)
{
	if (date) {
		g_autofree char *text = g_date_time_format_iso8601 (date);
		_pps_variant_add_string (builder, name, text);
	}
}

gboolean
_pps_variant_read_date (GVariant *dictionary, const char *name, GDateTime **out)
{
	g_autofree char *text = NULL;
	if (!_pps_variant_read_string (dictionary, name, &text))
		return FALSE;
	if (!text)
		return TRUE;
	*out = g_date_time_new_from_iso8601 (text, NULL);
	return *out != NULL;
}

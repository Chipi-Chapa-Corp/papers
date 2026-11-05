// SPDX-License-Identifier: GPL-2.0-or-later

#include <gio/gio.h>

#include "pps-document-info-private.h"
#include "pps-document-variant-private.h"

static const struct {
	const char *name;
	gsize offset;
} string_fields[] = {
	{ "title", G_STRUCT_OFFSET (PpsDocumentInfo, title) },
	{ "format", G_STRUCT_OFFSET (PpsDocumentInfo, format) },
	{ "author", G_STRUCT_OFFSET (PpsDocumentInfo, author) },
	{ "subject", G_STRUCT_OFFSET (PpsDocumentInfo, subject) },
	{ "keywords", G_STRUCT_OFFSET (PpsDocumentInfo, keywords) },
	{ "creator", G_STRUCT_OFFSET (PpsDocumentInfo, creator) },
	{ "producer", G_STRUCT_OFFSET (PpsDocumentInfo, producer) },
	{ "linearized", G_STRUCT_OFFSET (PpsDocumentInfo, linearized) },
	{ "security", G_STRUCT_OFFSET (PpsDocumentInfo, security) },
};

GVariant *
_pps_document_info_to_variant (const PpsDocumentInfo *info)
{
	GVariantBuilder builder;

	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
	for (guint i = 0; i < G_N_ELEMENTS (string_fields); i++) {
		char *const *value = G_STRUCT_MEMBER_P (info, string_fields[i].offset);
		_pps_variant_add_string (&builder, string_fields[i].name, *value);
	}
	g_variant_builder_add (&builder, "{sv}", "fields-mask", g_variant_new_uint32 (info->fields_mask));
	g_variant_builder_add (&builder, "{sv}", "layout", g_variant_new_int32 (info->layout));
	g_variant_builder_add (&builder, "{sv}", "mode", g_variant_new_int32 (info->mode));
	g_variant_builder_add (&builder, "{sv}", "ui-hints", g_variant_new_uint32 (info->ui_hints));
	g_variant_builder_add (&builder, "{sv}", "permissions", g_variant_new_uint32 (info->permissions));
	g_variant_builder_add (&builder, "{sv}", "n-pages", g_variant_new_int32 (info->n_pages));
	g_variant_builder_add (&builder, "{sv}", "contains-js", g_variant_new_int32 (info->contains_js));
	if (info->creation_datetime) {
		g_autofree char *date = g_date_time_format_iso8601 (info->creation_datetime);
		_pps_variant_add_string (&builder, "created", date);
	}
	if (info->modified_datetime) {
		g_autofree char *date = g_date_time_format_iso8601 (info->modified_datetime);
		_pps_variant_add_string (&builder, "modified", date);
	}
	if (info->page_sizes) {
		GVariantBuilder sizes;
		g_variant_builder_init (&sizes, G_VARIANT_TYPE ("a(dd)"));
		for (int i = 0; i < info->n_pages; i++)
			g_variant_builder_add (&sizes, "(dd)", info->page_sizes[i].width, info->page_sizes[i].height);
		g_variant_builder_add (&builder, "{sv}", "page-sizes", g_variant_builder_end (&sizes));
	}
	if (info->license) {
		GVariantBuilder license;
		g_variant_builder_init (&license, G_VARIANT_TYPE_VARDICT);
		_pps_variant_add_string (&license, "text", info->license->text);
		_pps_variant_add_string (&license, "uri", info->license->uri);
		_pps_variant_add_string (&license, "web-statement", info->license->web_statement);
		g_variant_builder_add (&builder, "{sv}", "license", g_variant_builder_end (&license));
	}
	return g_variant_builder_end (&builder);
}

static gboolean
read_date (GVariant *dictionary, const char *name, GDateTime **out)
{
	g_autofree char *text = NULL;
	if (!_pps_variant_read_string (dictionary, name, &text))
		return FALSE;
	if (!text)
		return TRUE;
	*out = g_date_time_new_from_iso8601 (text, NULL);
	return *out != NULL;
}

PpsDocumentInfo *
_pps_document_info_from_variant (GVariant *value, GError **error)
{
	g_autoptr (PpsDocumentInfo) info = pps_document_info_new ();
	g_autoptr (GVariant) sizes = NULL;
	g_autoptr (GVariant) license = NULL;
	int layout, mode, contains_js;

	if (!value || !g_variant_is_of_type (value, G_VARIANT_TYPE_VARDICT) ||
	    !g_variant_lookup (value, "fields-mask", "u", &info->fields_mask) ||
	    !g_variant_lookup (value, "layout", "i", &layout) ||
	    !g_variant_lookup (value, "mode", "i", &mode) ||
	    !g_variant_lookup (value, "ui-hints", "u", &info->ui_hints) ||
	    !g_variant_lookup (value, "permissions", "u", &info->permissions) ||
	    !g_variant_lookup (value, "n-pages", "i", &info->n_pages) ||
	    !g_variant_lookup (value, "contains-js", "i", &contains_js) ||
	    info->n_pages < 0)
		goto invalid;
	info->layout = layout;
	info->mode = mode;
	info->contains_js = contains_js;
	for (guint i = 0; i < G_N_ELEMENTS (string_fields); i++) {
		char **out = G_STRUCT_MEMBER_P (info, string_fields[i].offset);
		if (!_pps_variant_read_string (value, string_fields[i].name, out))
			goto invalid;
	}
	if (!read_date (value, "created", &info->creation_datetime) ||
	    !read_date (value, "modified", &info->modified_datetime))
		goto invalid;
	sizes = g_variant_lookup_value (value, "page-sizes", NULL);
	if (sizes) {
		if (!g_variant_is_of_type (sizes, G_VARIANT_TYPE ("a(dd)")) ||
		    g_variant_n_children (sizes) != (gsize) info->n_pages)
			goto invalid;
		info->page_sizes = g_new0 (PpsDocumentPageSize, info->n_pages);
		for (int i = 0; i < info->n_pages; i++)
			g_variant_get_child (sizes, i, "(dd)", &info->page_sizes[i].width, &info->page_sizes[i].height);
	}
	license = g_variant_lookup_value (value, "license", NULL);
	if (license) {
		if (!g_variant_is_of_type (license, G_VARIANT_TYPE_VARDICT))
			goto invalid;
		info->license = pps_document_license_new ();
		if (!_pps_variant_read_string (license, "text", &info->license->text) ||
		    !_pps_variant_read_string (license, "uri", &info->license->uri) ||
		    !_pps_variant_read_string (license, "web-statement", &info->license->web_statement))
			goto invalid;
	}
	return g_steal_pointer (&info);

invalid:
	g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
	                     "Invalid document information");
	return NULL;
}

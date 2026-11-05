// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-attachment-private.h"
#include "pps-document-buffer-private.h"
#include "pps-document-variant-private.h"

GVariant *
_pps_attachment_to_variant (PpsAttachment *attachment, GUnixFDList *fd_list, GError **error)
{
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
	if (attachment) {
		g_autoptr (GBytes) data = _pps_attachment_get_data (attachment);
		int handle = _pps_document_bytes_to_buffer (data, fd_list, error);
		if (handle == -1) {
			g_variant_builder_clear (&builder);
			return NULL;
		}
		_pps_variant_add_string (&builder, "name", pps_attachment_get_name (attachment));
		_pps_variant_add_string (&builder, "description", pps_attachment_get_description (attachment));
		_pps_variant_add_date (&builder, "modified", pps_attachment_get_modification_datetime (attachment));
		_pps_variant_add_date (&builder, "created", pps_attachment_get_creation_datetime (attachment));
		g_variant_builder_add (&builder, "{sv}", "buffer", g_variant_new ("(ht)", handle, (guint64) g_bytes_get_size (data)));
	}
	return g_variant_builder_end (&builder);
}

PpsAttachment *
_pps_attachment_from_variant (GVariant *value, GUnixFDList *fd_list, GError **error)
{
	g_autofree char *name = NULL, *description = NULL;
	g_autoptr (GDateTime) modified = NULL, created = NULL;
	g_autoptr (GBytes) data = NULL;
	int handle;
	guint64 size;
	if (!g_variant_is_of_type (value, G_VARIANT_TYPE_VARDICT))
		goto invalid;
	if (!g_variant_n_children (value))
		return NULL;
	if (!_pps_variant_read_string (value, "name", &name) ||
	    !_pps_variant_read_string (value, "description", &description) ||
	    !_pps_variant_read_date (value, "modified", &modified) ||
	    !_pps_variant_read_date (value, "created", &created) ||
	    !g_variant_lookup (value, "buffer", "(ht)", &handle, &size) || size > G_MAXUINT)
		goto invalid;
	data = _pps_document_bytes_from_buffer (handle, size, fd_list, error);
	if (!data)
		return NULL;
	return pps_attachment_new (name, description, modified, created, size,
	                           g_memdup2 (g_bytes_get_data (data, NULL), size));
invalid:
	g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid document attachment");
	return NULL;
}

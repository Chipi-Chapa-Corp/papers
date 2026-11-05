// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-attachment-private.h"
#include "pps-document-proxy-private.h"

static gboolean
has_attachments (PpsDocumentAttachments *document)
{
	GVariant *metadata = _pps_document_proxy_get_metadata (PPS_DOCUMENT (document));
	gboolean present = FALSE;
	if (metadata)
		g_variant_lookup (metadata, "has-attachments", "b", &present);
	return present;
}

static GList *
get_attachments (PpsDocumentAttachments *document)
{
	g_autoptr (PpsDocumentAttachments) hold = g_object_ref (document);
	PpsDocumentSession *session = _pps_document_proxy_get_session (PPS_DOCUMENT (document));
	g_autolist (PpsAttachment) attachments = NULL;
	guint offset = 0;
	gboolean more;
	do {
		g_autoptr (GVariant) values = NULL;
		g_autoptr (GUnixFDList) descriptors = NULL;
		g_autoptr (GError) error = NULL;
		if (!pps_dbus_document_call_get_attachments_sync (session->proxy, offset, G_DBUS_CALL_FLAGS_NONE, -1,
		                                                  NULL, &more, &values, &descriptors, NULL, &error))
			return NULL;
		gsize count = g_variant_n_children (values);
		if (count > PPS_DOCUMENT_BUFFER_BATCH_SIZE || (more && !count) || count > G_MAXUINT - offset)
			return NULL;
		for (gsize i = 0; i < count; i++) {
			g_autoptr (GVariant) value = g_variant_get_child_value (values, i);
			PpsAttachment *attachment = _pps_attachment_from_variant (value, descriptors, &error);
			if (!attachment)
				return NULL;
			attachments = g_list_prepend (attachments, attachment);
		}
		offset += count;
	} while (more);
	return g_list_reverse (g_steal_pointer (&attachments));
}

void
_pps_document_proxy_attachments_iface_init (PpsDocumentAttachmentsInterface *iface)
{
	iface->has_attachments = has_attachments;
	iface->get_attachments = get_attachments;
}

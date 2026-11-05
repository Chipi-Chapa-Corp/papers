// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-attachment-private.h"
#include "pps-document-worker-private.h"

static gboolean
get_attachments (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, GUnixFDList *input_fds, guint start, Worker *worker)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_ATTACHMENTS, invocation))
		return TRUE;
	if (!worker->attachments) {
		GList *attachments = pps_document_attachments_get_attachments (PPS_DOCUMENT_ATTACHMENTS (worker->document));
		worker->attachments = g_ptr_array_new_with_free_func (g_object_unref);
		for (GList *item = attachments; item; item = item->next)
			g_ptr_array_add (worker->attachments, item->data);
		g_list_free (attachments);
	}
	if (start > worker->attachments->len) {
		g_dbus_method_invocation_return_error_literal (
		    invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid document attachment offset");
		return TRUE;
	}
	guint end = start + MIN (PPS_DOCUMENT_ATTACHMENTS_BATCH_SIZE, worker->attachments->len - start);
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	g_autoptr (GError) error = NULL;
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));
	for (guint i = start; i < end; i++) {
		GVariant *value = _pps_attachment_to_variant (g_ptr_array_index (worker->attachments, i), descriptors, &error);
		if (!value) {
			g_variant_builder_clear (&builder);
			g_dbus_method_invocation_return_gerror (invocation, error);
			return TRUE;
		}
		g_variant_builder_add_value (&builder, value);
	}
	pps_dbus_document_complete_get_attachments (interface, invocation, descriptors,
	                                            end < worker->attachments->len, g_variant_builder_end (&builder));
	return TRUE;
}

void
pps_document_worker_bind_attachments (Worker *worker, PpsDBusDocument *interface)
{
	g_signal_connect (interface, "handle-get-attachments", G_CALLBACK (get_attachments), worker);
}

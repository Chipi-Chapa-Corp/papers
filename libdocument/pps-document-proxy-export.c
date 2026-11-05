// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include "pps-document-proxy-private.h"
#include "pps-file-exporter-private.h"

static PpsDBusDocument *
get_proxy (PpsFileExporter *exporter)
{
	return _pps_document_proxy_get_session (PPS_DOCUMENT (exporter))->proxy;
}

static gboolean
is_active (PpsFileExporter *exporter)
{
	return g_object_get_data (G_OBJECT (exporter), "pps-proxy-exporting") != NULL;
}

static void
end_export (PpsFileExporter *exporter)
{
	if (!is_active (exporter))
		return;
	g_autoptr (GError) error = NULL;
	if (!pps_dbus_document_call_export_end_sync (get_proxy (exporter), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error))
		_pps_file_exporter_take_error (exporter, g_steal_pointer (&error));
	g_object_set_data (G_OBJECT (exporter), "pps-proxy-exporting", NULL);
}

static void
begin_export (PpsFileExporter *exporter, PpsFileExporterContext *context)
{
	end_export (exporter);
	_pps_file_exporter_take_error (exporter, NULL);
	if (!context->filename) {
		_pps_file_exporter_take_error (exporter, g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Missing export filename"));
		return;
	}
	int fd = g_open (context->filename, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd == -1) {
		int saved_errno = errno;
		_pps_file_exporter_take_error (exporter, g_error_new_literal (G_IO_ERROR, g_io_error_from_errno (saved_errno), g_strerror (saved_errno)));
		return;
	}
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	g_autoptr (GError) error = NULL;
	int handle = g_unix_fd_list_append (descriptors, fd, &error);
	close (fd);
	if (handle == -1) {
		_pps_file_exporter_take_error (exporter, g_steal_pointer (&error));
		return;
	}
	if (pps_dbus_document_call_export_begin_sync (get_proxy (exporter), g_variant_new_handle (handle), context->format,
	                                              context->first_page, context->last_page, context->paper_width, context->paper_height,
	                                              context->duplex, context->pages_per_sheet, G_DBUS_CALL_FLAGS_NONE, -1,
	                                              descriptors, NULL, NULL, &error))
		g_object_set_data (G_OBJECT (exporter), "pps-proxy-exporting", GINT_TO_POINTER (TRUE));
	else
		_pps_file_exporter_take_error (exporter, g_steal_pointer (&error));
}

static void
begin_page (PpsFileExporter *exporter)
{
	g_autoptr (GError) error = NULL;
	if (is_active (exporter) && !_pps_file_exporter_get_error (exporter, NULL) &&
	    !pps_dbus_document_call_export_begin_page_sync (get_proxy (exporter), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error))
		_pps_file_exporter_take_error (exporter, g_steal_pointer (&error));
}

static void
do_page (PpsFileExporter *exporter, PpsRenderContext *context)
{
	g_autoptr (GError) error = NULL;
	if (is_active (exporter) && !_pps_file_exporter_get_error (exporter, NULL) &&
	    !pps_dbus_document_call_export_page_sync (get_proxy (exporter), context->page->index, context->rotation, context->scale,
	                                              context->target_width, context->target_height, context->annot_flags,
	                                              G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error))
		_pps_file_exporter_take_error (exporter, g_steal_pointer (&error));
}

static void
end_page (PpsFileExporter *exporter)
{
	g_autoptr (GError) error = NULL;
	if (is_active (exporter) && !_pps_file_exporter_get_error (exporter, NULL) &&
	    !pps_dbus_document_call_export_end_page_sync (get_proxy (exporter), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error))
		_pps_file_exporter_take_error (exporter, g_steal_pointer (&error));
}

static PpsFileExporterCapabilities
get_capabilities (PpsFileExporter *exporter)
{
	GVariant *metadata = _pps_document_proxy_get_metadata (PPS_DOCUMENT (exporter));
	guint capabilities = 0;
	if (metadata)
		g_variant_lookup (metadata, "export-capabilities", "u", &capabilities);
	return capabilities;
}

void
_pps_document_proxy_exporter_iface_init (PpsFileExporterInterface *iface)
{
	iface->begin = begin_export;
	iface->begin_page = begin_page;
	iface->do_page = do_page;
	iface->end_page = end_page;
	iface->end = end_export;
	iface->get_capabilities = get_capabilities;
}

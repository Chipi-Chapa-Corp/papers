// SPDX-License-Identifier: GPL-2.0-or-later

#include <fcntl.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pps-document-worker-private.h"
#include "pps-file-exporter-private.h"

void
pps_document_worker_clear_exporter (Worker *worker)
{
	if (worker->export_fd == -1)
		return;
	pps_file_exporter_end (PPS_FILE_EXPORTER (worker->document));
	close (worker->export_fd);
	worker->export_fd = -1;
}

static void
invalid_export (GDBusMethodInvocation *invocation)
{
	g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid document export operation");
}

static gboolean
return_export_error (Worker *worker, GDBusMethodInvocation *invocation)
{
	g_autoptr (GError) error = NULL;
	if (!_pps_file_exporter_get_error (PPS_FILE_EXPORTER (worker->document), &error))
		return FALSE;
	g_dbus_method_invocation_return_gerror (invocation, error);
	return TRUE;
}

static gboolean
begin_export (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, GUnixFDList *descriptors, GVariant *handle, guint format, int first, int last, double width, double height, gboolean duplex, int nup, Worker *worker)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_FILE_EXPORTER, invocation))
		return TRUE;
	PpsFileExporter *exporter = PPS_FILE_EXPORTER (worker->document);
	PpsFileExporterCapabilities capabilities = pps_file_exporter_get_capabilities (exporter);
	if (!((format == PPS_FILE_FORMAT_PS && (capabilities & PPS_FILE_EXPORTER_CAN_GENERATE_PS)) ||
	      (format == PPS_FILE_FORMAT_PDF && (capabilities & PPS_FILE_EXPORTER_CAN_GENERATE_PDF))) ||
	    first < 0 || last < first || last >= pps_document_get_n_pages (worker->document) ||
	    !isfinite (width) || !isfinite (height) || width <= 0 || height <= 0 || nup < 1 || nup > 16 ||
	    (nup != 1 && !(capabilities & PPS_FILE_EXPORTER_CAN_NUMBER_UP)) ||
	    !descriptors || g_variant_get_handle (handle) < 0 || g_variant_get_handle (handle) >= g_unix_fd_list_get_length (descriptors)) {
		invalid_export (invocation);
		return TRUE;
	}
	g_autoptr (GError) error = NULL;
	int fd = g_unix_fd_list_get (descriptors, g_variant_get_handle (handle), &error);
	if (fd == -1) {
		g_dbus_method_invocation_return_gerror (invocation, error);
		return TRUE;
	}
	struct stat statbuf;
	int flags = fcntl (fd, F_GETFL);
	if (fstat (fd, &statbuf) == -1 || !S_ISREG (statbuf.st_mode) || flags == -1 || (flags & O_ACCMODE) == O_RDONLY) {
		close (fd);
		invalid_export (invocation);
		return TRUE;
	}
	pps_document_worker_clear_exporter (worker);
	worker->export_fd = fd;
	g_autofree char *filename = g_strdup_printf ("/proc/self/fd/%d", fd);
	PpsFileExporterContext context = { format, filename, first, last, width, height, duplex, nup };
	pps_file_exporter_begin (exporter, &context);
	if (return_export_error (worker, invocation))
		pps_document_worker_clear_exporter (worker);
	else
		pps_dbus_document_complete_export_begin (interface, invocation, NULL);
	return TRUE;
}

static gboolean
check_export (Worker *worker, GDBusMethodInvocation *invocation)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_FILE_EXPORTER, invocation))
		return FALSE;
	if (worker->export_fd == -1) {
		g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED, "No document export is active");
		return FALSE;
	}
	return TRUE;
}

static gboolean
begin_page (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, Worker *worker)
{
	if (!check_export (worker, invocation))
		return TRUE;
	pps_file_exporter_begin_page (PPS_FILE_EXPORTER (worker->document));
	if (!return_export_error (worker, invocation))
		pps_dbus_document_complete_export_begin_page (interface, invocation);
	return TRUE;
}

static gboolean
do_page (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, int page, int rotation, double scale, int target_width, int target_height, guint annotations, Worker *worker)
{
	if (!check_export (worker, invocation))
		return TRUE;
	g_autoptr (PpsRenderContext) context = pps_document_worker_create_render_context (
	    worker, page, PPS_TYPE_FILE_EXPORTER, rotation, scale, target_width, target_height, annotations, invocation);
	if (!context)
		return TRUE;
	pps_file_exporter_do_page (PPS_FILE_EXPORTER (worker->document), context);
	if (!return_export_error (worker, invocation))
		pps_dbus_document_complete_export_page (interface, invocation);
	return TRUE;
}

static gboolean
end_page (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, Worker *worker)
{
	if (!check_export (worker, invocation))
		return TRUE;
	pps_file_exporter_end_page (PPS_FILE_EXPORTER (worker->document));
	if (!return_export_error (worker, invocation))
		pps_dbus_document_complete_export_end_page (interface, invocation);
	return TRUE;
}

static gboolean
end_export (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, Worker *worker)
{
	if (!check_export (worker, invocation))
		return TRUE;
	pps_document_worker_clear_exporter (worker);
	if (!return_export_error (worker, invocation))
		pps_dbus_document_complete_export_end (interface, invocation);
	return TRUE;
}

void
pps_document_worker_bind_exporter (Worker *worker, PpsDBusDocument *interface)
{
	g_signal_connect (interface, "handle-export-begin", G_CALLBACK (begin_export), worker);
	g_signal_connect (interface, "handle-export-begin-page", G_CALLBACK (begin_page), worker);
	g_signal_connect (interface, "handle-export-page", G_CALLBACK (do_page), worker);
	g_signal_connect (interface, "handle-export-end-page", G_CALLBACK (end_page), worker);
	g_signal_connect (interface, "handle-export-end", G_CALLBACK (end_export), worker);
}

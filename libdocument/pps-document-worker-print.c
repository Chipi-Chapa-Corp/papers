// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <adwaita.h>

#include "pps-document-worker-private.h"
#include "pps-print-operation-private.h"

static void
print_begun (PpsPrintOperation *op, PpsDBusDocument *interface)
{
	pps_dbus_document_emit_print_begun (interface);
}

static void
print_progress (PpsPrintOperation *op, PpsDBusDocument *interface)
{
	pps_dbus_document_emit_print_progress (interface, pps_print_operation_get_status (op),
	                                       pps_print_operation_get_progress (op));
}

static void
print_done (PpsPrintOperation *op, GtkPrintOperationResult result, Worker *worker)
{
	GDBusMethodInvocation *invocation = g_object_get_data (G_OBJECT (op), "print-invocation");
	PpsDBusDocument *interface = g_object_get_data (G_OBJECT (op), "print-interface");
	g_autoptr (GError) error = NULL;
	if (result == GTK_PRINT_OPERATION_RESULT_ERROR)
		pps_print_operation_get_error (op, &error);
	if (error)
		g_dbus_method_invocation_return_gerror (invocation, error);
	else
		pps_dbus_document_complete_print (
		    interface, invocation, result,
		    gtk_print_settings_to_gvariant (pps_print_operation_get_print_settings (op)),
		    gtk_page_setup_to_gvariant (pps_print_operation_get_default_page_setup (op)));
	g_clear_object (&worker->print_operation);
}

static gboolean
print_document (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, GVariant *settings_data, GVariant *setup_data, const char *title, int current_page, gboolean embed_page_setup, const char *parent_handle, Worker *worker)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_PRINT, invocation))
		return TRUE;
	if (worker->print_operation) {
		g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR, G_IO_ERROR_PENDING, "A print operation is already running");
		return TRUE;
	}
	if (current_page < 0 || current_page >= pps_document_get_n_pages (worker->document)) {
		g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid current print page");
		return TRUE;
	}
	g_autoptr (GtkPrintSettings) settings = gtk_print_settings_new_from_gvariant (settings_data);
	g_autoptr (GtkPageSetup) setup = gtk_page_setup_new_from_gvariant (setup_data);
	if (!settings || !setup) {
		g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid print settings");
		return TRUE;
	}
	if (!gtk_init_check ()) {
		g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot open the print display");
		return TRUE;
	}
	adw_init ();
	PpsPrintOperation *op = _pps_print_operation_new_for_worker (worker->document, parent_handle);
	worker->print_operation = G_OBJECT (op);
	g_object_set_data_full (G_OBJECT (op), "print-invocation", g_object_ref (invocation), g_object_unref);
	g_object_set_data_full (G_OBJECT (op), "print-interface", g_object_ref (interface), g_object_unref);
	pps_print_operation_set_current_page (op, current_page);
	pps_print_operation_set_print_settings (op, settings);
	pps_print_operation_set_default_page_setup (op, setup);
	pps_print_operation_set_job_name (op, title);
	pps_print_operation_set_embed_page_setup (op, embed_page_setup);
	g_signal_connect (op, "begin-print", G_CALLBACK (print_begun), interface);
	g_signal_connect (op, "status-changed", G_CALLBACK (print_progress), interface);
	g_signal_connect (op, "done", G_CALLBACK (print_done), worker);
	/* gtk_print_operation_run may finish before returning. */
	g_object_ref (op);
	pps_print_operation_run (op, NULL);
	g_object_unref (op);
	return TRUE;
}

static gboolean
cancel_print (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, Worker *worker)
{
	if (worker->print_operation)
		pps_print_operation_cancel (PPS_PRINT_OPERATION (worker->print_operation));
	pps_dbus_document_complete_cancel_print (interface, invocation);
	return TRUE;
}

void
pps_document_worker_bind_print (Worker *worker, PpsDBusDocument *interface)
{
	g_signal_connect (interface, "handle-print", G_CALLBACK (print_document), worker);
	g_signal_connect (interface, "handle-cancel-print", G_CALLBACK (cancel_print), worker);
}

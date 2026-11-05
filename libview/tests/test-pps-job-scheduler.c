// SPDX-License-Identifier: GPL-2.0-or-later

#include <signal.h>
#include <sys/wait.h>

#include <papers-view.h>

#include "pps-document-proxy-private.h"

static GDBusMessage *
count_requests (GDBusConnection *connection, GDBusMessage *message, gboolean incoming, gpointer data)
{
	if (!incoming && g_dbus_message_get_message_type (message) == G_DBUS_MESSAGE_TYPE_METHOD_CALL &&
	    g_strcmp0 (g_dbus_message_get_member (message), "ListAnnotations") == 0)
		g_atomic_int_inc ((gint *) data);
	return message;
}

static void
finished (PpsJob *job, gboolean *done)
{
	*done = TRUE;
}

static void
iterate (void)
{
	while (g_main_context_iteration (NULL, FALSE)) {
	}
	g_usleep (1000);
}

static PpsDocument *
open_document (const char *uri)
{
	g_autoptr (GError) error = NULL;
	PpsDocument *document = pps_document_factory_get_document (uri, &error);
	g_assert_no_error (error);
	g_assert_true (pps_document_load (document, uri, &error));
	g_assert_no_error (error);
	pps_document_setup_cache (document);
	return document;
}

static void
independent_documents (void)
{
	g_autofree char *uri = g_filename_to_uri (TESTDATADIR "/utf16le-annot.pdf", NULL, NULL);
	g_autoptr (PpsDocument) first = open_document (uri);
	g_autoptr (PpsDocument) second = open_document (uri);
	PpsDocumentSession *session = _pps_document_proxy_get_session (first);
	gint requests = 0;
	guint filter = g_dbus_connection_add_filter (session->connection, count_requests, &requests, NULL);
	pid_t pid = g_ascii_strtoll (g_subprocess_get_identifier (session->process), NULL, 10);
	siginfo_t status;
	g_subprocess_send_signal (session->process, SIGSTOP);
	g_assert_cmpint (waitid (P_PID, pid, &status, WSTOPPED | WNOWAIT), ==, 0);

	g_autoptr (PpsJob) blocked = pps_job_annots_new (first);
	pps_job_scheduler_push_job (blocked, PPS_JOB_PRIORITY_NONE);
	gint64 deadline = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;
	while (!g_atomic_int_get (&requests) && g_get_monotonic_time () < deadline)
		iterate ();
	gboolean started = g_atomic_int_get (&requests) == 1;

	g_autoptr (PpsJob) queued = pps_job_annots_new (first);
	gboolean queued_cancelled = FALSE;
	g_signal_connect (queued, "cancelled", G_CALLBACK (finished), &queued_cancelled);
	pps_job_scheduler_push_job (queued, PPS_JOB_PRIORITY_NONE);
	pps_job_scheduler_update_job (queued, PPS_JOB_PRIORITY_URGENT);
	pps_job_cancel (queued);
	g_autoptr (PpsJob) other = pps_job_annots_new (second);
	g_autoptr (PpsJob) load = pps_job_load_new ();
	pps_job_load_set_uri (PPS_JOB_LOAD (load), uri);
	gboolean other_done = FALSE, load_done = FALSE;
	g_signal_connect (other, "finished", G_CALLBACK (finished), &other_done);
	g_signal_connect (load, "finished", G_CALLBACK (finished), &load_done);
	pps_job_scheduler_push_job (other, PPS_JOB_PRIORITY_NONE);
	pps_job_scheduler_push_job (load, PPS_JOB_PRIORITY_NONE);
	deadline = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;
	while ((!other_done || !load_done) && g_get_monotonic_time () < deadline)
		iterate ();
	gboolean independent = other_done && load_done;
	gboolean serialized = g_atomic_int_get (&requests) == 1;
	/* Always release the stopped child before reporting a regression. */
	g_subprocess_send_signal (session->process, SIGCONT);
	pps_job_scheduler_wait ();
	while (g_main_context_iteration (NULL, FALSE)) {
	}
	g_dbus_connection_remove_filter (session->connection, filter);
	g_assert_true (started);
	g_assert_true (independent);
	g_assert_true (serialized);
	g_assert_true (pps_job_is_succeeded (blocked, NULL));
	g_assert_true (pps_job_is_succeeded (other, NULL));
	g_assert_true (pps_job_is_succeeded (load, NULL));
	g_assert_cmpuint (g_list_length (pps_job_annots_get_annots (PPS_JOB_ANNOTS (other))), ==, 2);
	g_assert_true (queued_cancelled);
	/* Updating a job after it left the scheduler must be harmless. */
	pps_job_scheduler_update_job (blocked, PPS_JOB_PRIORITY_URGENT);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_assert_true (pps_init ());
	g_test_add_func ("/job-scheduler/independent-documents", independent_documents);
	int result = g_test_run ();
	pps_shutdown ();
	return result;
}

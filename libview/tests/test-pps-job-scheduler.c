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

typedef struct {
	GSubprocess *process;
	GMutex mutex;
	GCond condition;
	gboolean finished, expired;
} WorkerGuard;

static gpointer
release_stopped_worker (gpointer data)
{
	WorkerGuard *guard = data;
	g_mutex_lock (&guard->mutex);
	gint64 deadline = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;
	while (!guard->finished && g_cond_wait_until (&guard->condition, &guard->mutex, deadline)) {
	}
	guard->expired = !guard->finished;
	if (guard->expired)
		g_subprocess_send_signal (guard->process, SIGCONT);
	g_mutex_unlock (&guard->mutex);
	return NULL;
}

static void
finish_guard (WorkerGuard *guard, GThread *thread)
{
	g_mutex_lock (&guard->mutex);
	guard->finished = TRUE;
	g_cond_signal (&guard->condition);
	g_mutex_unlock (&guard->mutex);
	g_thread_join (thread);
	g_mutex_clear (&guard->mutex);
	g_cond_clear (&guard->condition);
}

static void
stop_worker (GSubprocess *process)
{
	pid_t pid = g_ascii_strtoll (g_subprocess_get_identifier (process), NULL, 10);
	siginfo_t status;
	g_subprocess_send_signal (process, SIGSTOP);
	g_assert_cmpint (waitid (P_PID, pid, &status, WSTOPPED | WNOWAIT), ==, 0);
}

static void
cancel_running_job (void)
{
	g_autofree char *uri = g_filename_to_uri (TESTDATADIR "/utf16le-annot.pdf", NULL, NULL);
	g_autoptr (PpsDocument) document = open_document (uri);
	PpsDocumentSession *session = _pps_document_proxy_get_session (document);
	stop_worker (session->process);
	gint requests = 0;
	guint filter = g_dbus_connection_add_filter (session->connection, count_requests, &requests, NULL);
	g_autoptr (PpsJob) job = pps_job_annots_new (document);
	pps_job_scheduler_push_job (job, PPS_JOB_PRIORITY_NONE);
	gint64 deadline = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;
	while (!g_atomic_int_get (&requests) && g_get_monotonic_time () < deadline)
		iterate ();
	WorkerGuard guard = { .process = session->process };
	GThread *thread = g_thread_new ("worker-guard", release_stopped_worker, &guard);
	pps_job_cancel (job);
	pps_job_scheduler_wait ();
	finish_guard (&guard, thread);
	g_subprocess_send_signal (session->process, SIGCONT);
	g_dbus_connection_remove_filter (session->connection, filter);
	g_assert_cmpint (g_atomic_int_get (&requests), ==, 1);
	g_assert_false (guard.expired);
	g_autoptr (PpsPage) page = pps_document_get_page (document, 0);
	g_autolist (PpsAnnotation) annotations = pps_document_annotations_get_annotations (PPS_DOCUMENT_ANNOTATIONS (document), page);
	g_assert_cmpuint (g_list_length (annotations), ==, 2);
}

typedef struct {
	PpsDocument *document;
	WorkerGuard *guard;
	GMainLoop *loop;
	guint progress_source;
	gboolean progressed, responsive;
} ForegroundCall;

static gboolean
foreground_progress (gpointer data)
{
	ForegroundCall *call = data;
	call->progress_source = 0;
	call->progressed = TRUE;
	/* The caller's document reference may disappear in a reentered UI event. */
	g_clear_object (&call->document);
	g_subprocess_send_signal (call->guard->process, SIGCONT);
	return G_SOURCE_REMOVE;
}

static gboolean
foreground_call (gpointer data)
{
	ForegroundCall *call = data;
	g_autoptr (PpsPage) page = pps_document_get_page (call->document, 0);
	g_autolist (PpsAnnotation) annotations = pps_document_annotations_get_annotations (PPS_DOCUMENT_ANNOTATIONS (call->document), page);
	g_assert_cmpuint (g_list_length (annotations), ==, 2);
	call->responsive = call->progressed;
	g_main_loop_quit (call->loop);
	return G_SOURCE_REMOVE;
}

static void
foreground_responsiveness (void)
{
	g_autofree char *uri = g_filename_to_uri (TESTDATADIR "/utf16le-annot.pdf", NULL, NULL);
	PpsDocument *document = open_document (uri);
	g_autoptr (GSubprocess) process = g_object_ref (_pps_document_proxy_get_session (document)->process);
	stop_worker (process);
	WorkerGuard guard = { .process = process };
	GThread *thread = g_thread_new ("worker-guard", release_stopped_worker, &guard);
	g_autoptr (GMainLoop) loop = g_main_loop_new (NULL, FALSE);
	ForegroundCall call = { .document = document, .guard = &guard, .loop = loop };
	PpsDocument *remaining = document;
	g_object_add_weak_pointer (G_OBJECT (document), (gpointer *) &remaining);
	g_idle_add (foreground_call, &call);
	call.progress_source = g_idle_add (foreground_progress, &call);
	g_main_loop_run (loop);
	finish_guard (&guard, thread);
	if (call.progress_source)
		g_source_remove (call.progress_source);
	g_clear_object (&call.document);
	g_assert_false (guard.expired);
	g_assert_true (call.responsive);
	g_assert_null (remaining);
}

static void
cancel_thumbnail (void)
{
	g_autofree char *uri = g_filename_to_uri (TESTDATADIR "/utf16le-annot.pdf", NULL, NULL);
	g_autoptr (PpsDocument) document = open_document (uri);
	g_autoptr (PpsJob) job = pps_job_thumbnail_texture_new (document, 0, 0, 1.0);
	pps_job_cancel (job);
	pps_job_run (job);
	g_assert_null (pps_job_thumbnail_texture_get_texture (PPS_JOB_THUMBNAIL_TEXTURE (job)));
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_assert_true (pps_init ());
	g_test_add_func ("/job-scheduler/independent-documents", independent_documents);
	g_test_add_func ("/job-scheduler/cancel-running-job", cancel_running_job);
	g_test_add_func ("/job-scheduler/foreground-responsiveness", foreground_responsiveness);
	g_test_add_func ("/job-scheduler/cancel-thumbnail", cancel_thumbnail);
	int result = g_test_run ();
	pps_shutdown ();
	return result;
}

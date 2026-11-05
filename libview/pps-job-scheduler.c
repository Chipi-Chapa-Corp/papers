/* pps-job-scheduler.c
 *  this file is part of papers, a gnome document viewer
 *
 * Copyright (C) 2008 Carlos Garcia Campos <carlosgc@gnome.org>
 *
 * Papers is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Papers is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "pps-job-scheduler.h"
#include "pps-document-proxy-private.h"

#ifdef G_LOG_DOMAIN
#undef G_LOG_DOMAIN
#endif
#define G_LOG_DOMAIN "PpsJobScheduler"

typedef struct _PpsSchedulerJob {
	PpsJob *job;
	PpsJobPriority priority;
	GThreadPool *pool;
} PpsSchedulerJob;

static GMutex jobs_mutex;
static GCond jobs_finished;
static guint pending_jobs;

static gpointer
pps_jobs_hash_init (gpointer data)
{
	return g_hash_table_new (NULL, NULL);
}

static GHashTable *
pps_jobs_hash (void)
{
	static GOnce once_init = G_ONCE_INIT;
	g_once (&once_init, pps_jobs_hash_init, NULL);
	return once_init.retval;
}

static void
pps_scheduler_job_free (PpsSchedulerJob *job)
{
	if (!job)
		return;

	g_object_unref (job->job);
	g_free (job);
}

static void
pps_scheduler_job_destroy (PpsSchedulerJob *job)
{
	g_debug ("destroying job: %s", PPS_GET_TYPE_NAME (job->job));

	g_mutex_lock (&jobs_mutex);
	if (g_hash_table_lookup (pps_jobs_hash (), job->job) == job)
		g_hash_table_remove (pps_jobs_hash (), job->job);
	g_mutex_unlock (&jobs_mutex);
	pps_scheduler_job_free (job);
	g_mutex_lock (&jobs_mutex);
	if (--pending_jobs == 0)
		g_cond_broadcast (&jobs_finished);
	g_mutex_unlock (&jobs_mutex);
}

static void
pps_job_thread (PpsSchedulerJob *scheduler_job, gpointer data)
{
	PpsJob *job = scheduler_job->job;

	g_debug ("running thread for job: %s", PPS_GET_TYPE_NAME (job));

	if (!g_cancellable_is_cancelled (pps_job_get_cancellable (job)))
		pps_job_run (job);

	pps_scheduler_job_destroy (scheduler_job);
}

static gint
job_priority_compare (PpsSchedulerJob *a,
                      PpsSchedulerJob *b,
                      gpointer data)
{
	return (gint) a->priority - (gint) b->priority;
}

static gpointer
pps_job_scheduler_init (gpointer data)
{
	GThreadPool *pool = g_thread_pool_new_full ((GFunc) pps_job_thread, NULL,
	                                            (GDestroyNotify) pps_scheduler_job_destroy,
	                                            1, FALSE, NULL);

	g_thread_pool_set_sort_function (pool, (GCompareDataFunc) job_priority_compare, NULL);

	return pool;
}

static GThreadPool *
pps_thread_pool (void)
{
	static GOnce once_init = G_ONCE_INIT;
	g_once (&once_init, pps_job_scheduler_init, NULL);
	return once_init.retval;
}

static void
free_pool (gpointer data)
{
	/* Jobs retain their owner until they leave the pool. Do not wait for the
	 * current thread when it releases the last document reference. */
	g_thread_pool_free (data, FALSE, FALSE);
}

/* Called with jobs_mutex held. Keep native backends serialized, while each
 * proxy document and each independent load can wait on its own worker. */
static GThreadPool *
pool_for_job (PpsJob *job)
{
	PpsDocument *document = pps_job_get_document (job);
	GObject *owner = PPS_IS_JOB_LOAD (job) ? G_OBJECT (job) : PPS_IS_DOCUMENT_PROXY (document) ? G_OBJECT (document)
	                                                                                           : NULL;
	if (!owner)
		return pps_thread_pool ();
	GThreadPool *pool = g_object_get_data (owner, "pps-job-pool");
	if (!pool) {
		pool = pps_job_scheduler_init (NULL);
		g_object_set_data_full (owner, "pps-job-pool", pool, free_pool);
	}
	return pool;
}

static void
pps_job_queue_push (PpsSchedulerJob *job)
{
	g_debug ("pushing job: %s, priority: %d",
	         PPS_GET_TYPE_NAME (job->job), job->priority);

	g_mutex_lock (&jobs_mutex);
	job->pool = pool_for_job (job->job);
	g_hash_table_insert (pps_jobs_hash (), job->job, job);
	pending_jobs++;
	g_thread_pool_push (job->pool, job, NULL);
	g_mutex_unlock (&jobs_mutex);
}

void
pps_job_scheduler_push_job (PpsJob *job,
                            PpsJobPriority priority)
{
	PpsSchedulerJob *s_job;

	s_job = g_new0 (PpsSchedulerJob, 1);
	s_job->job = g_object_ref (job);
	s_job->priority = priority;

	pps_job_queue_push (s_job);
}

void
pps_job_scheduler_update_job (PpsJob *job,
                              PpsJobPriority priority)
{
	g_debug ("update priority for job: %s, priority %d",
	         PPS_GET_TYPE_NAME (job), priority);

	g_mutex_lock (&jobs_mutex);
	PpsSchedulerJob *scheduled = g_hash_table_lookup (pps_jobs_hash (), job);
	if (scheduled && priority == PPS_JOB_PRIORITY_URGENT)
		g_thread_pool_move_to_front (scheduled->pool, scheduled);
	g_mutex_unlock (&jobs_mutex);
}

/**
 * pps_job_scheduler_wait:
 *
 * Synchronously waits until all jobs are done.
 * Remember that main loop is not running already probably.
 */
void
pps_job_scheduler_wait (void)
{
	g_debug ("Waiting for empty job list");

	g_mutex_lock (&jobs_mutex);
	while (pending_jobs)
		g_cond_wait (&jobs_finished, &jobs_mutex);
	g_mutex_unlock (&jobs_mutex);

	g_debug ("Job list is empty");
}

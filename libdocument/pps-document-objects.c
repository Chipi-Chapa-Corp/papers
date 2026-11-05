// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-objects-private.h"

struct _PpsDocumentObjects {
	GHashTable *by_id;
	GHashTable *by_object;
	guint64 next_id;
	GRecMutex mutex;
	GObject *scope;
};

typedef struct {
	GObject *scope;
	guint64 id;
} ObjectHandle;

static GQuark
handle_quark (void)
{
	return g_quark_from_static_string ("pps-document-object-handle");
}

static void
free_handle (gpointer data)
{
	ObjectHandle *handle = data;
	g_object_unref (handle->scope);
	g_free (handle);
}

gboolean
_pps_document_objects_has_binding (GObject *object)
{
	return g_object_get_qdata (object, handle_quark ()) != NULL;
}

PpsDocumentObjects *
_pps_document_objects_new (void)
{
	PpsDocumentObjects *objects = g_new0 (PpsDocumentObjects, 1);
	objects->by_id = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, g_object_unref);
	objects->by_object = g_hash_table_new (g_direct_hash, g_direct_equal);
	objects->next_id = 1;
	objects->scope = g_object_new (G_TYPE_OBJECT, NULL);
	g_rec_mutex_init (&objects->mutex);
	return objects;
}

void
_pps_document_objects_free (PpsDocumentObjects *objects)
{
	g_hash_table_unref (objects->by_object);
	g_hash_table_unref (objects->by_id);
	g_object_unref (objects->scope);
	g_rec_mutex_clear (&objects->mutex);
	g_free (objects);
}

guint64
_pps_document_objects_get_id (PpsDocumentObjects *objects, GObject *object)
{
	if (!object)
		return 0;
	g_autoptr (GRecMutexLocker) locker = g_rec_mutex_locker_new (&objects->mutex);
	const guint64 *id = g_hash_table_lookup (objects->by_object, object);
	if (id)
		return *id;
	ObjectHandle *handle = g_object_get_qdata (object, handle_quark ());
	return handle && handle->scope == objects->scope ? handle->id : 0;
}

gboolean
_pps_document_objects_bind (PpsDocumentObjects *objects, guint64 id, GObject *object)
{
	g_autoptr (GRecMutexLocker) locker = g_rec_mutex_locker_new (&objects->mutex);
	ObjectHandle *previous = g_object_get_qdata (object, handle_quark ());
	if (previous)
		return id != 0 && previous->scope == objects->scope && previous->id == id;
	if (!id || g_hash_table_contains (objects->by_id, &id) || g_hash_table_contains (objects->by_object, object))
		return FALSE;
	ObjectHandle *handle = g_new (ObjectHandle, 1);
	handle->id = id;
	/* The token outlives the table if a wrapper survives its document. */
	handle->scope = g_object_ref (objects->scope);
	if (g_object_replace_qdata (object, handle_quark (), NULL, handle, free_handle, NULL))
		return TRUE;
	free_handle (handle);
	previous = g_object_get_qdata (object, handle_quark ());
	return previous && previous->scope == objects->scope && previous->id == id;
}

GObject *
_pps_document_objects_lookup (PpsDocumentObjects *objects, guint64 id, GType type)
{
	g_autoptr (GRecMutexLocker) locker = g_rec_mutex_locker_new (&objects->mutex);
	GObject *object = g_hash_table_lookup (objects->by_id, &id);
	return object && g_type_is_a (G_OBJECT_TYPE (object), type) ? object : NULL;
}

gboolean
_pps_document_objects_import (PpsDocumentObjects *objects, guint64 id, GObject *object)
{
	g_autoptr (GRecMutexLocker) locker = g_rec_mutex_locker_new (&objects->mutex);
	GObject *previous = g_hash_table_lookup (objects->by_id, &id);
	guint64 *key;
	if (!id || previous || _pps_document_objects_get_id (objects, object))
		return previous == object && id != 0;
	key = g_new (guint64, 1);
	*key = id;
	g_hash_table_insert (objects->by_id, key, g_object_ref (object));
	g_hash_table_insert (objects->by_object, object, key);
	return TRUE;
}

guint64
_pps_document_objects_export (PpsDocumentObjects *objects, GObject *object)
{
	g_autoptr (GRecMutexLocker) locker = g_rec_mutex_locker_new (&objects->mutex);
	guint64 id;
	if (!object)
		return 0;
	id = _pps_document_objects_get_id (objects, object);
	if (!id) {
		while (objects->next_id && g_hash_table_contains (objects->by_id, &objects->next_id))
			objects->next_id++;
		id = objects->next_id;
		if (!id)
			return 0;
		objects->next_id++;
		_pps_document_objects_import (objects, id, object);
	}
	return id;
}

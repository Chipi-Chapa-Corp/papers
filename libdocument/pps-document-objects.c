// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-objects-private.h"

struct _PpsDocumentObjects {
	GHashTable *by_id;
	GHashTable *by_object;
	guint64 next_id;
	GRecMutex mutex;
};

PpsDocumentObjects *
_pps_document_objects_new (void)
{
	PpsDocumentObjects *objects = g_new0 (PpsDocumentObjects, 1);
	objects->by_id = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, g_object_unref);
	objects->by_object = g_hash_table_new (g_direct_hash, g_direct_equal);
	objects->next_id = 1;
	g_rec_mutex_init (&objects->mutex);
	return objects;
}

void
_pps_document_objects_free (PpsDocumentObjects *objects)
{
	g_hash_table_unref (objects->by_object);
	g_hash_table_unref (objects->by_id);
	g_rec_mutex_clear (&objects->mutex);
	g_free (objects);
}

guint64
_pps_document_objects_get_id (PpsDocumentObjects *objects, GObject *object)
{
	g_autoptr (GRecMutexLocker) locker = g_rec_mutex_locker_new (&objects->mutex);
	const guint64 *id = g_hash_table_lookup (objects->by_object, object);
	return id ? *id : 0;
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

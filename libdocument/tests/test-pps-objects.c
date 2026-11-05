// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-objects-private.h"
#include "pps-layer.h"
#include "pps-link.h"

static void
identity_and_scope (void)
{
	g_autoptr (PpsDocumentObjects) worker = _pps_document_objects_new ();
	g_autoptr (PpsDocumentObjects) client = _pps_document_objects_new ();
	g_autoptr (PpsLayer) original = pps_layer_new (1);
	g_autoptr (PpsLayer) copy = pps_layer_new (1);
	guint64 id = _pps_document_objects_export (worker, G_OBJECT (original));
	g_assert_cmpuint (id, !=, 0);
	g_assert_cmpuint (_pps_document_objects_export (worker, G_OBJECT (original)), ==, id);
	g_assert_true (_pps_document_objects_lookup (worker, id, PPS_TYPE_LAYER) == G_OBJECT (original));
	g_assert_null (_pps_document_objects_lookup (worker, id, PPS_TYPE_LINK));
	g_assert_null (_pps_document_objects_lookup (worker, G_MAXUINT64, PPS_TYPE_LAYER));
	g_assert_cmpuint (_pps_document_objects_get_id (client, G_OBJECT (original)), ==, 0);
	g_assert_true (_pps_document_objects_import (client, id, G_OBJECT (copy)));
	g_assert_true (_pps_document_objects_lookup (client, id, PPS_TYPE_LAYER) == G_OBJECT (copy));
	g_assert_false (_pps_document_objects_import (client, id, G_OBJECT (original)));
	g_assert_false (_pps_document_objects_import (client, 0, G_OBJECT (original)));
	g_assert_true (_pps_document_objects_import (client, G_MAXUINT64, G_OBJECT (original)));
	g_assert_true (_pps_document_objects_lookup (client, G_MAXUINT64, PPS_TYPE_LAYER) == G_OBJECT (original));
}

static void
owned_lifetime (void)
{
	PpsDocumentObjects *objects = _pps_document_objects_new ();
	GObject *object = g_object_new (G_TYPE_OBJECT, NULL);
	GWeakRef weak;
	g_weak_ref_init (&weak, object);
	guint64 id = _pps_document_objects_export (objects, object);
	g_object_unref (object);
	g_autoptr (GObject) retained = g_weak_ref_get (&weak);
	g_assert_nonnull (retained);
	g_assert_true (_pps_document_objects_lookup (objects, id, G_TYPE_OBJECT) == retained);
	g_clear_object (&retained);
	_pps_document_objects_free (objects);
	g_assert_null (g_weak_ref_get (&weak));
	g_weak_ref_clear (&weak);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/document-objects/identity-and-scope", identity_and_scope);
	g_test_add_func ("/document-objects/owned-lifetime", owned_lifetime);
	return g_test_run ();
}

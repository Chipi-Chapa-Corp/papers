// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-annotation.h"
#include "pps-document-objects-private.h"
#include "pps-layer.h"
#include "pps-link.h"
#include "pps-media-private.h"
#include "pps-transition-effect.h"
#include <glib/gstdio.h>

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

typedef struct {
	PpsDocumentObjects *objects;
	GObject *object;
} ExportTask;

static gpointer
export_repeatedly (gpointer data)
{
	ExportTask *task = data;
	for (int i = 0; i < 1000; i++) {
		guint64 id = _pps_document_objects_export (task->objects, task->object);
		g_assert_cmpuint (id, !=, 0);
		g_assert_true (_pps_document_objects_lookup (task->objects, id, G_TYPE_OBJECT) == task->object);
	}
	return NULL;
}

static void
concurrent_identity (void)
{
	/* Exports can originate from a document job or the UI thread. */
	g_autoptr (PpsDocumentObjects) objects = _pps_document_objects_new ();
	g_autoptr (GObject) object = g_object_new (G_TYPE_OBJECT, NULL);
	ExportTask task = { objects, object };
	GThread *threads[4];
	for (guint i = 0; i < G_N_ELEMENTS (threads); i++)
		threads[i] = g_thread_new ("export-object", export_repeatedly, &task);
	for (guint i = 0; i < G_N_ELEMENTS (threads); i++)
		g_thread_join (threads[i]);
	g_assert_cmpuint (_pps_document_objects_get_id (objects, object), ==, 1);
}

static void
layer_title_lifetime (void)
{
	g_autoptr (PpsLayer) layer = pps_layer_new (0);
	g_object_set (layer, "title", "original", NULL);
	g_object_set (layer, "title", "replacement", NULL);
	g_autofree char *title = NULL;
	g_object_get (layer, "title", &title, NULL);
	g_assert_cmpstr (title, ==, "replacement");
	g_object_run_dispose (G_OBJECT (layer));
	g_object_run_dispose (G_OBJECT (layer));
}

static void
bound_wrapper_lifetime (void)
{
	g_autoptr (PpsDocumentObjects) first = _pps_document_objects_new ();
	g_autoptr (PpsDocumentObjects) second = _pps_document_objects_new ();
	g_autoptr (GObject) wrapper = g_object_new (G_TYPE_OBJECT, NULL);
	g_autoptr (GObject) alias = g_object_new (G_TYPE_OBJECT, NULL);
	g_assert_false (_pps_document_objects_has_binding (wrapper));
	g_assert_false (_pps_document_objects_bind (first, 0, wrapper));
	g_assert_true (_pps_document_objects_bind (first, 7, wrapper));
	g_assert_true (_pps_document_objects_has_binding (wrapper));
	g_assert_true (_pps_document_objects_bind (first, 7, alias));
	g_assert_true (_pps_document_objects_bind (first, 7, wrapper));
	g_assert_false (_pps_document_objects_bind (first, 8, wrapper));
	g_assert_false (_pps_document_objects_bind (second, 7, wrapper));
	g_assert_cmpuint (_pps_document_objects_get_id (first, wrapper), ==, 7);
	g_assert_cmpuint (_pps_document_objects_get_id (second, wrapper), ==, 0);
	g_assert_null (_pps_document_objects_lookup (first, 7, G_TYPE_OBJECT));
	GWeakRef weak;
	g_weak_ref_init (&weak, alias);
	g_clear_object (&alias);
	g_assert_null (g_weak_ref_get (&weak));
	g_weak_ref_clear (&weak);
	g_clear_pointer (&first, _pps_document_objects_free);
	first = _pps_document_objects_new ();
	/* Surviving wrappers cannot become valid in a subsequently opened document. */
	g_assert_cmpuint (_pps_document_objects_get_id (first, wrapper), ==, 0);
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

static void
free_text_font_property (void)
{
	g_autoptr (PpsPage) page = pps_page_new (0);
	g_autoptr (PpsAnnotation) annotation = pps_annotation_free_text_new (page);
	g_autoptr (PangoFontDescription) font = pango_font_description_from_string ("Serif Bold Italic 14");
	g_autoptr (PangoFontDescription) copy = NULL;
	g_object_set (annotation, "font-desc", font, NULL);
	g_object_get (annotation, "font-desc", &copy, NULL);
	g_assert_nonnull (copy);
	g_assert_true (pango_font_description_equal (font, copy));
	/* Boxed property reads own their copy, independently of the annotation. */
	g_clear_object (&annotation);
	g_assert_cmpstr (pango_font_description_get_family (copy), ==, "Serif");
}

static void
stamp_surface_lifetime (void)
{
	g_autoptr (PpsPage) page = pps_page_new (0);
	g_autoptr (PpsAnnotation) annotation = pps_annotation_stamp_new (page);
	cairo_surface_t *first = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 2, 2);
	cairo_surface_t *second = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 2, 2);
	pps_annotation_stamp_set_surface (PPS_ANNOTATION_STAMP (annotation), first);
	pps_annotation_stamp_set_surface (PPS_ANNOTATION_STAMP (annotation), first);
	g_assert_cmpuint (cairo_surface_get_reference_count (first), ==, 2);
	pps_annotation_stamp_set_surface (PPS_ANNOTATION_STAMP (annotation), second);
	g_assert_cmpuint (cairo_surface_get_reference_count (first), ==, 1);
	g_assert_true (pps_annotation_stamp_get_surface (PPS_ANNOTATION_STAMP (annotation)) == second);
	g_clear_object (&annotation);
	g_assert_cmpuint (cairo_surface_get_reference_count (second), ==, 1);
	cairo_surface_destroy (first);
	cairo_surface_destroy (second);
}

static void
transition_rectangular_property (void)
{
	g_autoptr (PpsTransitionEffect) effect = pps_transition_effect_new (PPS_TRANSITION_EFFECT_FLY, "rectangular", TRUE, NULL);
	gboolean rectangular = FALSE;
	g_object_get (effect, "rectangular", &rectangular, NULL);
	g_assert_true (rectangular);
	g_object_set (effect, "rectangular", FALSE, NULL);
	g_object_get (effect, "rectangular", &rectangular, NULL);
	g_assert_false (rectangular);
}

static void
media_temp_file_lifetime (void)
{
	g_autoptr (GError) error = NULL;
	g_autofree char *directory = g_dir_make_tmp ("papers-media-test-XXXXXX", &error);
	g_assert_no_error (error);
	g_autofree char *path = g_build_filename (directory, "media", NULL);
	g_assert_true (g_file_set_contents (path, "media", -1, &error));
	g_assert_no_error (error);
	g_autoptr (GFile) file = g_file_new_for_path (path);
	g_autofree char *uri = g_file_get_uri (file);
	g_autoptr (PpsPage) page = pps_page_new (1);
	g_autoptr (PpsMedia) first = pps_media_new_for_uri (page, uri);
	g_autoptr (PpsMedia) second = pps_media_new_for_uri (page, uri);
	g_assert_null (_pps_media_get_temp_file (first));
	_pps_media_set_temp_file (first, file);
	_pps_media_set_temp_file (first, file);
	_pps_media_set_temp_file (second, file);
	g_assert_true (_pps_media_get_temp_file (first) == file);
	g_assert_true (_pps_media_get_temp_file (second) == file);
	g_clear_object (&file);
	g_clear_object (&first);
	g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));
	g_clear_object (&second);
	g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/document-objects/identity-and-scope", identity_and_scope);
	g_test_add_func ("/document-objects/owned-lifetime", owned_lifetime);
	g_test_add_func ("/document-objects/concurrent-identity", concurrent_identity);
	g_test_add_func ("/document-objects/layer-title-lifetime", layer_title_lifetime);
	g_test_add_func ("/document-objects/bound-wrapper-lifetime", bound_wrapper_lifetime);
	g_test_add_func ("/document-objects/free-text-font-property", free_text_font_property);
	g_test_add_func ("/document-objects/stamp-surface-lifetime", stamp_surface_lifetime);
	g_test_add_func ("/document-objects/transition-rectangular-property", transition_rectangular_property);
	g_test_add_func ("/document-objects/media-temp-file-lifetime", media_temp_file_lifetime);
	return g_test_run ();
}

// SPDX-License-Identifier: GPL-2.0-or-later

#include <glib/gstdio.h>
#include <papers-view.h>

#include "pps-document-factory-private.h"
#include "pps-document-proxy-private.h"

static void
undo_and_save (gconstpointer data)
{
	g_autoptr (GError) error = NULL;
	gboolean proxy = GPOINTER_TO_INT (data);
	g_autoptr (PpsDocument) document = proxy ? _pps_document_proxy_new ("application/pdf", NULL, &error) : _pps_document_factory_create_document ("application/pdf", &error);
	g_assert_no_error (error);
	g_autofree char *uri = g_filename_to_uri (TESTDATADIR "/utf16le-annot.pdf", NULL, NULL);
	g_assert_true (pps_document_load (document, uri, &error));
	g_assert_no_error (error);
	pps_document_setup_cache (document);
	g_autoptr (PpsAnnotationModel) annotation_model = pps_annotation_model_new ();
	g_autoptr (PpsDocumentModel) model = g_object_new (PPS_TYPE_DOCUMENT_MODEL,
	                                                   "annotation-model", annotation_model, "document", document, NULL);
	g_autoptr (PpsUndoContext) undo = pps_undo_context_new (model);
	g_autoptr (PpsAnnotationsContext) context = pps_annotations_context_new (model, undo);
	GListModel *annotations = pps_annotations_context_get_annots_model (context);
	gint64 deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
	while (g_list_model_get_n_items (annotations) < 2 && g_get_monotonic_time () < deadline) {
		while (g_main_context_iteration (NULL, FALSE)) {
		}
		g_usleep (1000);
	}
	guint original_count = g_list_model_get_n_items (annotations);
	g_assert_cmpuint (original_count, ==, 2);
	PpsPoint start = { 30, 30 };
	GdkRGBA color = { 1, .8, .1, 1 };
	g_autoptr (PpsAnnotation) note = pps_annotations_context_add_annotation_sync (
	    context, 0, PPS_ANNOTATION_TYPE_TEXT, &start, &start, &color, NULL);
	g_assert_nonnull (note);
	g_assert_cmpuint (g_list_model_get_n_items (annotations), ==, original_count + 1);
	guint64 id = proxy ? _pps_document_objects_get_id (_pps_document_proxy_get_objects (document), G_OBJECT (note)) : 0;
	if (proxy)
		g_assert_cmpuint (id, !=, 0);
	pps_annotation_set_contents (note, "Undo through worker");
	pps_undo_context_undo (undo);
	g_assert_cmpstr (pps_annotation_get_contents (note), ==, "");
	pps_undo_context_redo (undo);
	g_assert_cmpstr (pps_annotation_get_contents (note), ==, "Undo through worker");
	pps_annotations_context_remove_annotation (context, note);
	g_assert_cmpuint (g_list_model_get_n_items (annotations), ==, original_count);
	pps_undo_context_undo (undo);
	g_assert_cmpuint (g_list_model_get_n_items (annotations), ==, original_count + 1);
	pps_undo_context_redo (undo);
	g_assert_cmpuint (g_list_model_get_n_items (annotations), ==, original_count);
	pps_undo_context_undo (undo);
	if (proxy)
		g_assert_cmpuint (_pps_document_objects_get_id (_pps_document_proxy_get_objects (document), G_OBJECT (note)), ==, id);
	g_autofree char *directory = g_dir_make_tmp ("papers-annotation-undo-XXXXXX", &error);
	g_assert_no_error (error);
	g_autofree char *saved_path = g_build_filename (directory, "saved.pdf", NULL);
	g_autofree char *saved_uri = g_filename_to_uri (saved_path, NULL, NULL);
	g_assert_true (pps_document_save (document, saved_uri, &error));
	g_assert_no_error (error);
	g_assert_false (pps_document_annotations_document_is_modified (PPS_DOCUMENT_ANNOTATIONS (document)));
	g_autoptr (PpsDocument) saved = proxy ? _pps_document_proxy_new ("application/pdf", NULL, &error) : _pps_document_factory_create_document ("application/pdf", &error);
	g_assert_no_error (error);
	g_assert_true (pps_document_load (saved, saved_uri, &error));
	g_assert_no_error (error);
	pps_document_setup_cache (saved);
	g_autoptr (PpsPage) page = pps_document_get_page (saved, 0);
	g_autolist (PpsAnnotation) saved_annotations = pps_document_annotations_get_annotations (PPS_DOCUMENT_ANNOTATIONS (saved), page);
	g_assert_cmpuint (g_list_length (saved_annotations), ==, original_count + 1);
	gboolean found = FALSE;
	for (GList *item = saved_annotations; item; item = item->next)
		found |= g_strcmp0 (pps_annotation_get_contents (item->data), "Undo through worker") == 0;
	g_assert_true (found);
	g_assert_cmpint (g_unlink (saved_path), ==, 0);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_assert_true (pps_init ());
	g_test_add_data_func ("/annotations/native-undo-and-save", GINT_TO_POINTER (FALSE), undo_and_save);
	g_test_add_data_func ("/annotations/proxy-undo-and-save", GINT_TO_POINTER (TRUE), undo_and_save);
	int result = g_test_run ();
	pps_shutdown ();
	return result;
}

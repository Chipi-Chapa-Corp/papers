// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-tree-private.h"
#include "pps-outlines.h"

static GVariant *
encode (GObject *object, gpointer data)
{
	g_autofree char *markup = NULL;
	GVariantBuilder builder;
	g_object_get (object, "markup", &markup, NULL);
	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add (&builder, "{sv}", "markup", g_variant_new_string (markup));
	return g_variant_builder_end (&builder);
}

static GObject *
decode (GVariant *value, gpointer data, GError **error)
{
	const char *markup;
	if (!g_variant_lookup (value, "markup", "&s", &markup)) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Missing outline markup");
		return NULL;
	}
	return g_object_new (PPS_TYPE_OUTLINES, "markup", markup, NULL);
}

static void
set_children (GObject *object, GListModel *children)
{
	pps_outlines_set_children (PPS_OUTLINES (object), children);
}

static void
deep_tree (void)
{
	g_autoptr (GListStore) roots = g_list_store_new (PPS_TYPE_OUTLINES);
	g_autoptr (PpsOutlines) root = g_object_new (PPS_TYPE_OUTLINES, "markup", "root", NULL);
	g_autoptr (PpsOutlines) parent = g_object_ref (root);
	g_list_store_append (roots, root);
	for (int i = 1; i <= 200; i++) {
		g_autofree char *title = g_strdup_printf ("node %d", i);
		g_autoptr (PpsOutlines) child = g_object_new (PPS_TYPE_OUTLINES, "markup", title, NULL);
		g_autoptr (GListStore) children = g_list_store_new (PPS_TYPE_OUTLINES);
		g_list_store_append (children, child);
		pps_outlines_set_children (parent, G_LIST_MODEL (children));
		g_set_object (&parent, child);
	}
	g_autoptr (GListStore) empty = g_list_store_new (PPS_TYPE_OUTLINES);
	pps_outlines_set_children (parent, G_LIST_MODEL (empty));
	g_autoptr (PpsOutlines) leaf = g_object_new (PPS_TYPE_OUTLINES, "markup", "leaf", NULL);
	g_list_store_append (roots, leaf);
	g_autoptr (GError) error = NULL;
	g_autoptr (GVariant) value = g_variant_ref_sink (_pps_document_tree_to_variant (G_LIST_MODEL (roots), encode, NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpuint (g_variant_n_children (value), ==, 202);
	g_autoptr (GListModel) copy = _pps_document_tree_from_variant (value, PPS_TYPE_OUTLINES, decode, set_children, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (g_list_model_get_n_items (copy), ==, 2);
	g_autoptr (PpsOutlines) item = g_list_model_get_item (copy, 0);
	for (int i = 1; i <= 200; i++) {
		GListModel *children = pps_outlines_get_children (item);
		g_assert_nonnull (children);
		g_assert_cmpuint (g_list_model_get_n_items (children), ==, 1);
		PpsOutlines *child = g_list_model_get_item (children, 0);
		g_set_object (&item, child);
		g_object_unref (child);
	}
	g_assert_nonnull (pps_outlines_get_children (item));
	g_assert_cmpuint (g_list_model_get_n_items (pps_outlines_get_children (item)), ==, 0);
	g_autoptr (PpsOutlines) copied_leaf = g_list_model_get_item (copy, 1);
	g_assert_null (pps_outlines_get_children (copied_leaf));
}

static void
invalid_parent (void)
{
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(iba{sv})"));
	g_autoptr (PpsOutlines) item = g_object_new (PPS_TYPE_OUTLINES, "markup", "bad", NULL);
	g_variant_builder_add (&builder, "(ib@a{sv})", 0, FALSE, encode (G_OBJECT (item), NULL));
	g_autoptr (GVariant) value = g_variant_ref_sink (g_variant_builder_end (&builder));
	g_autoptr (GError) error = NULL;
	g_autoptr (GListModel) result = _pps_document_tree_from_variant (value, PPS_TYPE_OUTLINES, decode, set_children, NULL, &error);
	g_assert_null (result);
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
cycle (void)
{
	g_autoptr (GListStore) roots = g_list_store_new (PPS_TYPE_OUTLINES);
	g_autoptr (PpsOutlines) item = g_object_new (PPS_TYPE_OUTLINES, "markup", "cycle", NULL);
	g_list_store_append (roots, item);
	pps_outlines_set_children (item, G_LIST_MODEL (roots));
	g_autoptr (GError) error = NULL;
	g_autoptr (GVariant) value = _pps_document_tree_to_variant (G_LIST_MODEL (roots), encode, NULL, &error);
	g_assert_null (value);
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	pps_outlines_set_children (item, NULL);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/document-tree/deep", deep_tree);
	g_test_add_func ("/document-tree/invalid-parent", invalid_parent);
	g_test_add_func ("/document-tree/cycle", cycle);
	return g_test_run ();
}

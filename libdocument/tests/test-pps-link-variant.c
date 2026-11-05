// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-link-private.h"
#include "pps-document-mapping-private.h"
#include "pps-document-tree-private.h"

static GVariant *
encode_layer (GObject *object, gpointer data)
{
	return _pps_layer_to_variant (PPS_LAYER (object), data);
}

static void
layer_tree_and_action_identity (void)
{
	g_autoptr (PpsDocumentObjects) worker = _pps_document_objects_new ();
	g_autoptr (PpsDocumentObjects) client = _pps_document_objects_new ();
	g_autoptr (PpsLayer) group = pps_layer_new (0);
	g_autoptr (PpsLayer) child = pps_layer_new (7);
	g_object_set (group, "title", "Group", "title-only", TRUE, NULL);
	g_object_set (child, "title", "Child", "enabled", TRUE, NULL);
	g_autoptr (GListStore) roots = g_list_store_new (PPS_TYPE_LAYER);
	g_autoptr (GListStore) children = g_list_store_new (PPS_TYPE_LAYER);
	g_list_store_append (roots, group);
	g_list_store_append (children, child);
	pps_layer_set_children (group, G_LIST_MODEL (children));
	g_autoptr (GError) error = NULL;
	g_autoptr (GVariant) value = g_variant_ref_sink (_pps_document_tree_to_variant (G_LIST_MODEL (roots), encode_layer, worker, &error));
	g_assert_no_error (error);
	g_autoptr (GListModel) copy = _pps_layers_from_variant (value, client, &error);
	g_assert_no_error (error);
	g_autoptr (PpsLayer) copied_group = g_list_model_get_item (copy, 0);
	g_autoptr (PpsLayer) copied_child = g_list_model_get_item (pps_layer_get_children (copied_group), 0);
	g_assert_cmpint (pps_layer_get_rb_group (copied_child), ==, 7);
	g_autoptr (PpsLinkAction) action = pps_link_action_new_layers_state (g_list_append (NULL, g_object_ref (child)), NULL, NULL);
	g_autoptr (PpsLink) link = pps_link_new (NULL, action);
	g_autoptr (GVariant) link_value = g_variant_ref_sink (_pps_link_to_variant (link, worker));
	g_autoptr (PpsLink) copied_link = _pps_link_from_variant (link_value, client, &error);
	g_assert_no_error (error);
	g_assert_true (pps_link_action_get_show_list (pps_link_get_action (copied_link))->data == copied_child);
}

typedef struct {
	PpsDocumentObjects *objects;
	GWeakRef *weak;
} MappingContext;

static GObject *
decode_mapping_link (GVariant *value, gpointer data, GError **error)
{
	MappingContext *context = data;
	PpsLink *link = _pps_link_from_variant (value, context->objects, error);
	if (link)
		g_weak_ref_set (context->weak, G_OBJECT (link));
	return G_OBJECT (link);
}

static void
invalid_mapping_releases_partial_result (void)
{
	g_autoptr (PpsDocumentObjects) worker = _pps_document_objects_new ();
	g_autoptr (PpsDocumentObjects) client = _pps_document_objects_new ();
	g_autoptr (PpsLinkAction) action = pps_link_action_new_external_uri ("https://example.org");
	g_autoptr (PpsLink) link = pps_link_new (NULL, action);
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a((dddd)a{sv})"));
	g_variant_builder_add (&builder, "((dddd)@a{sv})", 0., 0., 1., 1., _pps_link_to_variant (link, worker));
	g_variant_builder_add (&builder, "((dddd)@a{sv})", 0., 0., NAN, 1., _pps_link_to_variant (link, worker));
	g_autoptr (GVariant) value = g_variant_ref_sink (g_variant_builder_end (&builder));
	GWeakRef weak;
	g_weak_ref_init (&weak, NULL);
	MappingContext context = { client, &weak };
	g_autoptr (GError) error = NULL;
	g_assert_null (_pps_mapping_list_from_variant (value, 0, decode_mapping_link, &context, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_assert_null (g_weak_ref_get (&weak));
	g_weak_ref_clear (&weak);
}
#include <math.h>

static void
destinations (void)
{
	g_autoptr (GPtrArray) destinations = g_ptr_array_new_with_free_func (g_object_unref);
	g_ptr_array_add (destinations, pps_link_dest_new_page (3));
	g_ptr_array_add (destinations, pps_link_dest_new_xyz (4, 1.25, 8.5, 1.75, TRUE, FALSE, TRUE));
	g_ptr_array_add (destinations, pps_link_dest_new_fit (1));
	g_ptr_array_add (destinations, pps_link_dest_new_fith (2, 3.5, FALSE));
	g_ptr_array_add (destinations, pps_link_dest_new_fitv (3, 4.5, TRUE));
	g_ptr_array_add (destinations, pps_link_dest_new_fitr (4, -1, -2, 30, 40));
	g_ptr_array_add (destinations, pps_link_dest_new_named ("chapter-α"));
	g_ptr_array_add (destinations, pps_link_dest_new_page_label ("iv"));
	for (guint i = 0; i < destinations->len; i++) {
		PpsLinkDest *original = destinations->pdata[i];
		g_autoptr (GVariant) value = g_variant_ref_sink (_pps_link_dest_to_variant (original));
		g_autoptr (GError) error = NULL;
		g_autoptr (PpsLinkDest) copy = _pps_link_dest_from_variant (value, &error);
		g_assert_no_error (error);
		g_assert_nonnull (copy);
		g_test_message ("destination type %d", pps_link_dest_get_dest_type (original));
		g_assert_true (pps_link_dest_equal (original, copy));
	}
}

static void
actions_and_identity (void)
{
	g_autoptr (PpsDocumentObjects) worker = _pps_document_objects_new ();
	g_autoptr (PpsDocumentObjects) client = _pps_document_objects_new ();
	g_autoptr (PpsLinkDest) dest = pps_link_dest_new_xyz (1, 10, 20, 2, TRUE, TRUE, FALSE);
	g_autoptr (GPtrArray) actions = g_ptr_array_new_with_free_func (g_object_unref);
	g_ptr_array_add (actions, pps_link_action_new_dest (dest));
	g_ptr_array_add (actions, pps_link_action_new_remote (dest, "other.pdf"));
	g_ptr_array_add (actions, pps_link_action_new_external_uri ("https://example.org/α"));
	g_ptr_array_add (actions, pps_link_action_new_launch ("file.txt", "parameters"));
	g_ptr_array_add (actions, pps_link_action_new_named ("NextPage"));
	g_autoptr (PpsLayer) layer = pps_layer_new (2);
	g_object_set (layer, "title", "Layer", "enabled", TRUE, NULL);
	g_ptr_array_add (actions, pps_link_action_new_layers_state (
				      g_list_append (NULL, g_object_ref (layer)), NULL,
				      g_list_append (NULL, g_object_ref (layer))));
	GList *fields = g_list_append (NULL, g_strdup ("first"));
	fields = g_list_append (fields, g_strdup ("second"));
	g_ptr_array_add (actions, pps_link_action_new_reset_form (fields, TRUE));
	for (guint i = 0; i < actions->len; i++) {
		g_autoptr (PpsLink) original = pps_link_new (i ? "Link" : NULL, actions->pdata[i]);
		g_autoptr (GVariant) value = g_variant_ref_sink (_pps_link_to_variant (original, worker));
		g_autoptr (GError) error = NULL;
		g_autoptr (PpsLink) copy = _pps_link_from_variant (value, client, &error);
		g_assert_no_error (error);
		g_assert_nonnull (copy);
		g_autoptr (GVariant) encoded = g_variant_ref_sink (_pps_link_to_variant (copy, client));
		g_assert_true (g_variant_equal (value, encoded));
		PpsLinkAction *action = pps_link_get_action (copy);
		if (i == PPS_LINK_ACTION_TYPE_LAYERS_STATE) {
			g_assert_true (pps_link_action_get_show_list (action)->data == pps_link_action_get_toggle_list (action)->data);
			g_assert_cmpint (pps_layer_get_rb_group (pps_link_action_get_show_list (action)->data), ==, 2);
		}
		if (i == PPS_LINK_ACTION_TYPE_RESET_FORM) {
			g_assert_true (pps_link_action_get_exclude_reset_fields (action));
			g_assert_cmpstr (pps_link_action_get_reset_fields (action)->data, ==, "first");
			g_assert_cmpstr (pps_link_action_get_reset_fields (action)->next->data, ==, "second");
		}
	}
}

static void
invalid_destination_and_handle (void)
{
	g_autoptr (PpsLinkDest) dest = pps_link_dest_new_page (0);
	g_autoptr (GVariant) value = g_variant_ref_sink (_pps_link_dest_to_variant (dest));
	GVariantDict dictionary;
	g_variant_dict_init (&dictionary, value);
	g_variant_dict_insert (&dictionary, "coordinates", "(ddddd)", NAN, 0., 0., 0., 0.);
	g_autoptr (GVariant) invalid = g_variant_ref_sink (g_variant_dict_end (&dictionary));
	g_autoptr (GError) error = NULL;
	g_assert_null (_pps_link_dest_from_variant (invalid, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_clear_error (&error);
	g_autoptr (PpsDocumentObjects) worker = _pps_document_objects_new ();
	g_autoptr (PpsDocumentObjects) client = _pps_document_objects_new ();
	g_autoptr (PpsLayer) layer = pps_layer_new (0);
	g_autoptr (GVariant) layer_value = g_variant_ref_sink (_pps_layer_to_variant (layer, worker));
	g_autoptr (PpsLink) foreign = pps_link_new (NULL, NULL);
	guint64 id = _pps_document_objects_get_id (worker, G_OBJECT (layer));
	g_assert_true (_pps_document_objects_import (client, id, G_OBJECT (foreign)));
	g_assert_null (_pps_layer_from_variant (layer_value, client, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/document-link/destinations", destinations);
	g_test_add_func ("/document-link/actions-and-identity", actions_and_identity);
	g_test_add_func ("/document-link/invalid", invalid_destination_and_handle);
	g_test_add_func ("/document-link/layer-tree-identity", layer_tree_and_action_identity);
	g_test_add_func ("/document-link/invalid-mapping", invalid_mapping_releases_partial_result);
	return g_test_run ();
}

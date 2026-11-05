// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-tree-private.h"

typedef struct {
	GObject *object;
	int parent;
} ModelItem;

static void
clear_item (gpointer data)
{
	ModelItem *item = data;
	g_object_unref (item->object);
}

static void
invalid_model (GError **error)
{
	if (!error || !*error)
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid document tree model");
}

GVariant *
_pps_document_tree_to_variant (GListModel *model, PpsDocumentEncodeItem encode, gpointer data, GError **error)
{
	g_autoptr (GArray) queue = g_array_new (FALSE, FALSE, sizeof (ModelItem));
	GVariantBuilder builder;
	g_array_set_clear_func (queue, clear_item);
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(iba{sv})"));
	for (guint i = 0; model && i < g_list_model_get_n_items (model); i++) {
		ModelItem item = { g_list_model_get_item (model, i), -1 };
		g_array_append_val (queue, item);
	}
	for (guint i = 0; i < queue->len; i++) {
		ModelItem item = g_array_index (queue, ModelItem, i);
		g_autoptr (GListModel) children = NULL;
		for (int parent = item.parent; parent >= 0; parent = g_array_index (queue, ModelItem, parent).parent) {
			if (g_array_index (queue, ModelItem, parent).object == item.object)
				goto invalid;
		}
		g_object_get (item.object, "children", &children, NULL);
		g_variant_builder_add (&builder, "(ib@a{sv})", item.parent, children != NULL, encode (item.object, data));
		for (guint j = 0; children && j < g_list_model_get_n_items (children); j++) {
			if (queue->len >= G_MAXINT)
				goto invalid;
			ModelItem child = { g_list_model_get_item (children, j), i };
			g_array_append_val (queue, child);
		}
	}
	return g_variant_builder_end (&builder);
invalid:
	g_variant_builder_clear (&builder);
	invalid_model (error);
	return NULL;
}

static void
clear_store (gpointer data)
{
	if (data)
		g_object_unref (data);
}

GListModel *
_pps_document_tree_from_variant (GVariant *value, GType item_type, PpsDocumentDecodeItem decode, PpsDocumentSetChildren set_children, gpointer data, GError **error)
{
	g_autoptr (GListStore) roots = g_list_store_new (item_type);
	g_autoptr (GArray) items = g_array_new (FALSE, FALSE, sizeof (ModelItem));
	g_autoptr (GPtrArray) stores = g_ptr_array_new_with_free_func (clear_store);
	g_array_set_clear_func (items, clear_item);
	if (!g_variant_is_of_type (value, G_VARIANT_TYPE ("a(iba{sv})")) ||
	    g_variant_n_children (value) > G_MAXINT)
		goto invalid;
	for (gsize i = 0; i < g_variant_n_children (value); i++) {
		int parent;
		gboolean has_children;
		g_autoptr (GVariant) properties = NULL;
		g_autoptr (GObject) object = NULL;
		GListStore *children = NULL;
		g_variant_get_child (value, i, "(ib@a{sv})", &parent, &has_children, &properties);
		if (parent < -1 || parent >= (int) i || (parent >= 0 && !stores->pdata[parent]))
			goto invalid;
		object = decode (properties, data, error);
		if (!object || !g_type_is_a (G_OBJECT_TYPE (object), item_type))
			goto invalid;
		for (int ancestor = parent; ancestor >= 0; ancestor = g_array_index (items, ModelItem, ancestor).parent) {
			if (g_array_index (items, ModelItem, ancestor).object == object)
				goto invalid;
		}
		if (has_children) {
			children = g_list_store_new (item_type);
			set_children (object, G_LIST_MODEL (children));
		}
		g_list_store_append (parent < 0 ? roots : G_LIST_STORE (stores->pdata[parent]), object);
		ModelItem item = { g_steal_pointer (&object), parent };
		g_array_append_val (items, item);
		g_ptr_array_add (stores, children);
	}
	return G_LIST_MODEL (g_steal_pointer (&roots));
invalid:
	invalid_model (error);
	return NULL;
}

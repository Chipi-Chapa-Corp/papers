// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-link-private.h"
#include "pps-document-mapping-private.h"
#include "pps-document-tree-private.h"
#include "pps-document-worker-private.h"

static void
clear_mapping (gpointer data)
{
	if (data)
		pps_mapping_list_unref (data);
}

void
pps_document_worker_clear_links (Worker *worker)
{
	g_clear_object (&worker->outlines);
	g_clear_object (&worker->layers);
	g_clear_pointer (&worker->links, g_hash_table_unref);
}

static gboolean
get_outlines (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, Worker *worker)
{
	g_autoptr (GError) error = NULL;
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_LINKS, invocation))
		return TRUE;
	if (!worker->outlines_loaded) {
		worker->outlines = pps_document_links_get_links_model (PPS_DOCUMENT_LINKS (worker->document));
		worker->outlines_loaded = TRUE;
	}
	GVariant *rows = _pps_outlines_to_variant (worker->outlines, worker->objects, &error);
	if (!rows)
		g_dbus_method_invocation_return_gerror (invocation, error);
	else
		pps_dbus_document_complete_get_outlines (interface, invocation, worker->outlines != NULL, rows);
	return TRUE;
}

static GVariant *
encode_link (GObject *link, gpointer data)
{
	return _pps_link_to_variant (PPS_LINK (link), data);
}

static gboolean
get_links (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, int index, Worker *worker)
{
	if (!pps_document_worker_check_page (worker, index, PPS_TYPE_DOCUMENT_LINKS, invocation))
		return TRUE;
	if (!worker->links)
		worker->links = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, clear_mapping);
	if (!g_hash_table_contains (worker->links, GINT_TO_POINTER (index))) {
		g_autoptr (PpsPage) page = pps_document_get_page (worker->document, index);
		g_hash_table_insert (worker->links, GINT_TO_POINTER (index),
		                     pps_document_links_get_links (PPS_DOCUMENT_LINKS (worker->document), page));
	}
	PpsMappingList *mappings = g_hash_table_lookup (worker->links, GINT_TO_POINTER (index));
	pps_dbus_document_complete_get_links (interface, invocation, mappings != NULL,
	                                      _pps_mapping_list_to_variant (mappings, encode_link, worker->objects));
	return TRUE;
}

static gboolean
find_link_dest (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, const char *name, Worker *worker)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_LINKS, invocation))
		return TRUE;
	g_autoptr (PpsLinkDest) dest = pps_document_links_find_link_dest (PPS_DOCUMENT_LINKS (worker->document), name);
	pps_dbus_document_complete_find_link_dest (interface, invocation, _pps_link_dest_to_variant (dest));
	return TRUE;
}

static gboolean
find_link_page (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, const char *name, Worker *worker)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_LINKS, invocation))
		return TRUE;
	pps_dbus_document_complete_find_link_page (interface, invocation,
	                                           pps_document_links_find_link_page (PPS_DOCUMENT_LINKS (worker->document), name));
	return TRUE;
}

static GVariant *
encode_layer (GObject *object, gpointer data)
{
	Worker *worker = data;
	gboolean title_only;
	g_object_get (object, "title-only", &title_only, NULL);
	if (!title_only)
		g_object_set (object, "enabled", pps_document_layers_layer_is_visible (PPS_DOCUMENT_LAYERS (worker->document), PPS_LAYER (object)), NULL);
	return _pps_layer_to_variant (PPS_LAYER (object), worker->objects);
}

static gboolean
get_layers (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, Worker *worker)
{
	g_autoptr (GError) error = NULL;
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_LAYERS, invocation))
		return TRUE;
	if (!worker->layers_loaded) {
		worker->layers = pps_document_layers_get_layers (PPS_DOCUMENT_LAYERS (worker->document));
		worker->layers_loaded = TRUE;
	}
	GVariant *rows = _pps_document_tree_to_variant (worker->layers, encode_layer, worker, &error);
	if (!rows)
		g_dbus_method_invocation_return_gerror (invocation, error);
	else
		pps_dbus_document_complete_get_layers (interface, invocation, worker->layers != NULL, rows);
	return TRUE;
}

static PpsLayer *
lookup_layer (Worker *worker, guint64 id, GDBusMethodInvocation *invocation)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_LAYERS, invocation))
		return NULL;
	PpsLayer *layer = (PpsLayer *) _pps_document_objects_lookup (worker->objects, id, PPS_TYPE_LAYER);
	gboolean title_only = TRUE;
	if (layer)
		g_object_get (layer, "title-only", &title_only, NULL);
	if (title_only) {
		g_dbus_method_invocation_return_error_literal (
		    invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid document layer handle");
		return NULL;
	}
	return layer;
}

static gboolean
set_visibility (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, guint64 id, gboolean visible, Worker *worker)
{
	PpsLayer *layer = lookup_layer (worker, id, invocation);
	if (!layer)
		return TRUE;
	if (visible)
		pps_document_layers_show_layer (PPS_DOCUMENT_LAYERS (worker->document), layer);
	else
		pps_document_layers_hide_layer (PPS_DOCUMENT_LAYERS (worker->document), layer);
	pps_dbus_document_complete_set_layer_visibility (interface, invocation);
	return TRUE;
}

static gboolean
get_visibility (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, guint64 id, Worker *worker)
{
	PpsLayer *layer = lookup_layer (worker, id, invocation);
	if (layer)
		pps_dbus_document_complete_get_layer_visibility (interface, invocation,
		                                                 pps_document_layers_layer_is_visible (PPS_DOCUMENT_LAYERS (worker->document), layer));
	return TRUE;
}

void
pps_document_worker_bind_links (Worker *worker, PpsDBusDocument *interface)
{
	g_signal_connect (interface, "handle-get-outlines", G_CALLBACK (get_outlines), worker);
	g_signal_connect (interface, "handle-get-links", G_CALLBACK (get_links), worker);
	g_signal_connect (interface, "handle-find-link-dest", G_CALLBACK (find_link_dest), worker);
	g_signal_connect (interface, "handle-find-link-page", G_CALLBACK (find_link_page), worker);
	g_signal_connect (interface, "handle-get-layers", G_CALLBACK (get_layers), worker);
	g_signal_connect (interface, "handle-set-layer-visibility", G_CALLBACK (set_visibility), worker);
	g_signal_connect (interface, "handle-get-layer-visibility", G_CALLBACK (get_visibility), worker);
}

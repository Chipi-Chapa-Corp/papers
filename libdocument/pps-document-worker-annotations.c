// SPDX-License-Identifier: GPL-2.0-or-later

#include <math.h>

#include "pps-document-annotation-private.h"
#include "pps-document-worker-private.h"

/* Records keep logical identity while the backend recreates a removed annotation. */
#define ANNOTATION_KEY "pps-worker-annotation"
#define ACTIVE_KEY "pps-worker-annotation-active"

static GObject *
new_record (PpsAnnotation *annotation)
{
	GObject *record = g_object_new (G_TYPE_OBJECT, NULL);
	g_object_set_data_full (record, ANNOTATION_KEY, annotation, g_object_unref);
	g_object_set_data (record, ACTIVE_KEY, GINT_TO_POINTER (TRUE));
	return record;
}

static GPtrArray *
page_annotations (Worker *worker, int index)
{
	if (!worker->annotations)
		worker->annotations = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) g_ptr_array_unref);
	GPtrArray *annotations = g_hash_table_lookup (worker->annotations, GINT_TO_POINTER (index));
	if (!annotations) {
		g_autoptr (PpsPage) page = pps_document_get_page (worker->document, index);
		GList *list = pps_document_annotations_get_annotations (PPS_DOCUMENT_ANNOTATIONS (worker->document), page);
		annotations = g_ptr_array_new_with_free_func (g_object_unref);
		for (GList *item = list; item; item = item->next)
			g_ptr_array_add (annotations, new_record (item->data));
		g_list_free (list);
		g_hash_table_insert (worker->annotations, GINT_TO_POINTER (index), annotations);
	}
	return annotations;
}

static void
invalid_annotation (GDBusMethodInvocation *invocation)
{
	g_dbus_method_invocation_return_error_literal (
	    invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid document annotation operation");
}

static GObject *
lookup_record (Worker *worker, guint64 id, gboolean active, GDBusMethodInvocation *invocation)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_ANNOTATIONS, invocation))
		return NULL;
	GObject *record = _pps_document_objects_lookup (worker->objects, id, G_TYPE_OBJECT);
	if (!record || !g_object_get_data (record, ANNOTATION_KEY) ||
	    (active && !g_object_get_data (record, ACTIVE_KEY))) {
		invalid_annotation (invocation);
		return NULL;
	}
	return record;
}

static gboolean
list_annotations (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, int index, Worker *worker)
{
	if (!pps_document_worker_check_page (worker, index, PPS_TYPE_DOCUMENT_ANNOTATIONS, invocation))
		return TRUE;
	GPtrArray *annotations = page_annotations (worker, index);
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("at"));
	for (guint i = 0; i < annotations->len; i++)
		g_variant_builder_add (&builder, "t", _pps_document_objects_export (worker->objects, g_ptr_array_index (annotations, i)));
	pps_dbus_document_complete_list_annotations (interface, invocation, g_variant_builder_end (&builder));
	return TRUE;
}

static gboolean
get_annotations (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, GUnixFDList *input_fds, int index, GVariant *handles, Worker *worker)
{
	if (!pps_document_worker_check_page (worker, index, PPS_TYPE_DOCUMENT_ANNOTATIONS, invocation))
		return TRUE;
	gsize count = g_variant_n_children (handles);
	if (count > PPS_DOCUMENT_BUFFER_BATCH_SIZE) {
		invalid_annotation (invocation);
		return TRUE;
	}
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	g_autoptr (GError) error = NULL;
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ta{sv})"));
	for (gsize i = 0; i < count; i++) {
		guint64 id;
		g_variant_get_child (handles, i, "t", &id);
		GObject *record = lookup_record (worker, id, FALSE, invocation);
		if (!record) {
			g_variant_builder_clear (&builder);
			return TRUE;
		}
		PpsAnnotation *annotation = g_object_get_data (record, ANNOTATION_KEY);
		if (pps_annotation_get_page_index (annotation) != (guint) index) {
			g_variant_builder_clear (&builder);
			invalid_annotation (invocation);
			return TRUE;
		}
		GVariant *value = _pps_annotation_to_variant (annotation, descriptors, &error);
		if (!value) {
			g_variant_builder_clear (&builder);
			g_dbus_method_invocation_return_gerror (invocation, error);
			return TRUE;
		}
		g_variant_builder_add (&builder, "(t@a{sv})", id, value);
	}
	pps_dbus_document_complete_get_annotations (interface, invocation, descriptors, g_variant_builder_end (&builder));
	return TRUE;
}

static gboolean
set_property (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, GUnixFDList *input_fds, guint64 id, const char *name, GVariant *value, Worker *worker)
{
	GObject *record = lookup_record (worker, id, TRUE, invocation);
	if (!record)
		return TRUE;
	g_autoptr (GError) error = NULL;
	g_autoptr (GVariant) property = g_variant_get_variant (value);
	if (!_pps_annotation_set_property_from_variant (g_object_get_data (record, ANNOTATION_KEY), name, property, input_fds, &error))
		g_dbus_method_invocation_return_gerror (invocation, error);
	else
		pps_dbus_document_complete_set_annotation_property (interface, invocation, NULL, pps_document_worker_get_state (worker));
	return TRUE;
}

static gboolean
add_annotation (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, GUnixFDList *input_fds, guint64 id, GVariant *value, Worker *worker)
{
	int index;
	if (!g_variant_lookup (value, "page", "i", &index)) {
		invalid_annotation (invocation);
		return TRUE;
	}
	if (!pps_document_worker_check_page (worker, index, PPS_TYPE_DOCUMENT_ANNOTATIONS, invocation))
		return TRUE;
	if (!pps_document_annotations_can_add_annotation (PPS_DOCUMENT_ANNOTATIONS (worker->document))) {
		g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Cannot add annotations");
		return TRUE;
	}
	GObject *record = id ? lookup_record (worker, id, FALSE, invocation) : NULL;
	if (id && !record)
		return TRUE;
	if (record && g_object_get_data (record, ACTIVE_KEY)) {
		invalid_annotation (invocation);
		return TRUE;
	}
	g_autoptr (PpsPage) page = pps_document_get_page (worker->document, index);
	g_autoptr (GError) error = NULL;
	g_autoptr (PpsAnnotation) annotation = _pps_annotation_from_variant (value, page, input_fds, &error);
	if (!annotation) {
		g_dbus_method_invocation_return_gerror (invocation, error);
		return TRUE;
	}
	PpsAnnotation *previous = record ? g_object_get_data (record, ANNOTATION_KEY) : NULL;
	g_autoptr (PangoFontDescription) font = NULL;
	if (PPS_IS_ANNOTATION_FREE_TEXT (annotation))
		g_object_get (annotation, "font-desc", &font, NULL);
	if ((previous && (G_OBJECT_TYPE (previous) != G_OBJECT_TYPE (annotation) || pps_annotation_get_page_index (previous) != (guint) index)) ||
	    PPS_IS_ANNOTATION_ATTACHMENT (annotation) ||
	    (PPS_IS_ANNOTATION_INK (annotation) && !pps_annotation_ink_get_ink_list (PPS_ANNOTATION_INK (annotation))) ||
	    (PPS_IS_ANNOTATION_FREE_TEXT (annotation) && !font)) {
		invalid_annotation (invocation);
		return TRUE;
	}
	/* Cache existing annotations before adding, so the backend cannot list this one twice. */
	GPtrArray *annotations = page_annotations (worker, index);
	pps_document_annotations_add_annotation (PPS_DOCUMENT_ANNOTATIONS (worker->document), annotation);
	if (record) {
		g_object_set_data_full (record, ANNOTATION_KEY, g_object_ref (annotation), g_object_unref);
		g_object_set_data (record, ACTIVE_KEY, GINT_TO_POINTER (TRUE));
		g_ptr_array_add (annotations, g_object_ref (record));
	} else {
		record = new_record (g_object_ref (annotation));
		g_ptr_array_add (annotations, record);
		id = _pps_document_objects_export (worker->objects, record);
	}
	PpsRectangle area;
	pps_annotation_get_area (annotation, &area);
	const char *name = pps_annotation_get_name (annotation);
	pps_dbus_document_complete_add_annotation (interface, invocation, NULL, id, name != NULL, name ? name : "",
	                                           g_variant_new ("(dddd)", area.x1, area.y1, area.x2, area.y2),
	                                           pps_document_worker_get_state (worker));
	return TRUE;
}

static gboolean
remove_annotation (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, guint64 id, Worker *worker)
{
	GObject *record = lookup_record (worker, id, TRUE, invocation);
	if (!record)
		return TRUE;
	if (!pps_document_annotations_can_remove_annotation (PPS_DOCUMENT_ANNOTATIONS (worker->document))) {
		g_dbus_method_invocation_return_error_literal (invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Cannot remove annotations");
		return TRUE;
	}
	PpsAnnotation *annotation = g_object_get_data (record, ANNOTATION_KEY);
	pps_document_annotations_remove_annotation (PPS_DOCUMENT_ANNOTATIONS (worker->document), annotation);
	g_object_set_data (record, ACTIVE_KEY, NULL);
	g_ptr_array_remove (page_annotations (worker, pps_annotation_get_page_index (annotation)), record);
	pps_dbus_document_complete_remove_annotation (interface, invocation, pps_document_worker_get_state (worker));
	return TRUE;
}

static gboolean
over_markup (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, guint64 id, double x, double y, Worker *worker)
{
	GObject *record = lookup_record (worker, id, TRUE, invocation);
	if (!record)
		return TRUE;
	if (!isfinite (x) || !isfinite (y)) {
		invalid_annotation (invocation);
		return TRUE;
	}
	pps_dbus_document_complete_over_annotation_markup (interface, invocation,
	                                                   pps_document_annotations_over_markup (PPS_DOCUMENT_ANNOTATIONS (worker->document),
	                                                                                         g_object_get_data (record, ANNOTATION_KEY), x, y));
	return TRUE;
}

void
pps_document_worker_bind_annotations (Worker *worker, PpsDBusDocument *interface)
{
	g_signal_connect (interface, "handle-list-annotations", G_CALLBACK (list_annotations), worker);
	g_signal_connect (interface, "handle-get-annotations", G_CALLBACK (get_annotations), worker);
	g_signal_connect (interface, "handle-set-annotation-property", G_CALLBACK (set_property), worker);
	g_signal_connect (interface, "handle-add-annotation", G_CALLBACK (add_annotation), worker);
	g_signal_connect (interface, "handle-remove-annotation", G_CALLBACK (remove_annotation), worker);
	g_signal_connect (interface, "handle-over-annotation-markup", G_CALLBACK (over_markup), worker);
}

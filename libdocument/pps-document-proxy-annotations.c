// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-annotation-private.h"
#include "pps-document-geometry-private.h"
#include "pps-document-proxy-private.h"

static void
annotation_changed (PpsAnnotation *annotation, GParamSpec *spec, PpsDocument *document)
{
	g_autoptr (PpsDocument) hold = g_object_ref (document);
	if ((spec->flags & G_PARAM_READWRITE) != G_PARAM_READWRITE || (spec->flags & G_PARAM_CONSTRUCT_ONLY))
		return;
	g_autoptr (GError) error = NULL;
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	GVariant *encoded = _pps_annotation_property_to_variant (annotation, spec->name, descriptors, &error);
	if (!encoded)
		return;
	g_autoptr (GVariant) value = g_variant_ref_sink (encoded);
	guint state;
	guint64 id = _pps_document_objects_get_id (_pps_document_proxy_get_objects (document), G_OBJECT (annotation));
	if (pps_dbus_document_call_set_annotation_property_sync (
		_pps_document_proxy_get_session (document)->proxy, id, spec->name, g_variant_new_variant (value),
		G_DBUS_CALL_FLAGS_NONE, -1, descriptors, &state, NULL, NULL, &error))
		_pps_document_proxy_set_state (document, state);
}

static gboolean
bind_annotation (PpsDocument *document, PpsAnnotation *annotation, guint64 id)
{
	if (!_pps_document_objects_bind (_pps_document_proxy_get_objects (document), id, G_OBJECT (annotation)))
		return FALSE;
	g_signal_handlers_disconnect_by_func (annotation, annotation_changed, document);
	g_signal_connect_object (annotation, "notify", G_CALLBACK (annotation_changed), document, 0);
	return TRUE;
}

static GList *
get_annotations (PpsDocumentAnnotations *annotations, PpsPage *page)
{
	g_autoptr (PpsDocumentAnnotations) hold = g_object_ref (annotations);
	PpsDocument *document = PPS_DOCUMENT (annotations);
	g_autolist (PpsAnnotation) list = NULL;
	g_autoptr (GVariant) handles = NULL;
	g_autoptr (GError) error = NULL;
	PpsDBusDocument *proxy = _pps_document_proxy_get_session (document)->proxy;
	if (!pps_dbus_document_call_list_annotations_sync (proxy, page->index, G_DBUS_CALL_FLAGS_NONE, -1, &handles, NULL, &error))
		return NULL;
	gsize total = g_variant_n_children (handles);
	for (gsize offset = 0; offset < total;) {
		gsize count = MIN (PPS_DOCUMENT_BUFFER_BATCH_SIZE, total - offset);
		GVariantBuilder builder;
		g_variant_builder_init (&builder, G_VARIANT_TYPE ("at"));
		for (gsize i = 0; i < count; i++) {
			guint64 id;
			g_variant_get_child (handles, offset + i, "t", &id);
			g_variant_builder_add (&builder, "t", id);
		}
		g_autoptr (GVariant) batch = g_variant_ref_sink (g_variant_builder_end (&builder));
		g_autoptr (GVariant) values = NULL;
		g_autoptr (GUnixFDList) descriptors = NULL;
		if (!pps_dbus_document_call_get_annotations_sync (proxy, page->index, batch, G_DBUS_CALL_FLAGS_NONE, -1,
		                                                  NULL, &values, &descriptors, NULL, &error) ||
		    g_variant_n_children (values) != count)
			return NULL;
		for (gsize i = 0; i < count; i++) {
			guint64 id, expected;
			g_autoptr (GVariant) value = NULL;
			g_variant_get_child (values, i, "(t@a{sv})", &id, &value);
			g_variant_get_child (handles, offset + i, "t", &expected);
			g_autoptr (PpsAnnotation) annotation = _pps_annotation_from_variant (value, page, descriptors, &error);
			if (id != expected || !annotation || !bind_annotation (document, annotation, id))
				return NULL;
			list = g_list_prepend (list, g_steal_pointer (&annotation));
		}
		offset += count;
	}
	return g_list_reverse (g_steal_pointer (&list));
}

static gboolean
is_modified (PpsDocumentAnnotations *annotations)
{
	return (_pps_document_proxy_get_state (PPS_DOCUMENT (annotations)) & PPS_DOCUMENT_STATE_ANNOTATIONS_MODIFIED) != 0;
}

static void
add_annotation (PpsDocumentAnnotations *annotations, PpsAnnotation *annotation)
{
	g_autoptr (PpsDocumentAnnotations) hold = g_object_ref (annotations);
	PpsDocument *document = PPS_DOCUMENT (annotations);
	PpsDocumentObjects *objects = _pps_document_proxy_get_objects (document);
	guint64 id = _pps_document_objects_get_id (objects, G_OBJECT (annotation));
	if (!id && _pps_document_objects_has_binding (G_OBJECT (annotation)))
		return;
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	g_autoptr (GError) error = NULL;
	GVariant *encoded = _pps_annotation_to_variant (annotation, descriptors, &error);
	if (!encoded)
		return;
	g_autoptr (GVariant) value = g_variant_ref_sink (encoded);
	g_autoptr (GVariant) area_value = NULL;
	g_autofree char *name = NULL;
	gboolean present;
	guint64 handle;
	guint state;
	if (!pps_dbus_document_call_add_annotation_sync (
		_pps_document_proxy_get_session (document)->proxy, id, value, G_DBUS_CALL_FLAGS_NONE, -1,
		descriptors, &handle, &present, &name, &area_value, &state, NULL, NULL, &error))
		return;
	PpsRectangle area;
	if (!_pps_rectangle_from_variant (area_value, &area) || !handle || (id && handle != id))
		return;
	g_signal_handlers_disconnect_by_func (annotation, annotation_changed, document);
	pps_annotation_set_name (annotation, present ? name : NULL);
	pps_annotation_set_area (annotation, &area);
	if (bind_annotation (document, annotation, handle))
		_pps_document_proxy_set_state (document, state);
}

static void
remove_annotation (PpsDocumentAnnotations *annotations, PpsAnnotation *annotation)
{
	g_autoptr (PpsDocumentAnnotations) hold = g_object_ref (annotations);
	PpsDocument *document = PPS_DOCUMENT (annotations);
	guint64 id = _pps_document_objects_get_id (_pps_document_proxy_get_objects (document), G_OBJECT (annotation));
	guint state;
	if (pps_dbus_document_call_remove_annotation_sync (_pps_document_proxy_get_session (document)->proxy, id,
	                                                   G_DBUS_CALL_FLAGS_NONE, -1, &state, NULL, NULL)) {
		g_signal_handlers_disconnect_by_func (annotation, annotation_changed, document);
		_pps_document_proxy_set_state (document, state);
	}
}

static PpsAnnotationsOverMarkup
over_markup (PpsDocumentAnnotations *annotations, PpsAnnotation *annotation, double x, double y)
{
	g_autoptr (PpsDocumentAnnotations) hold = g_object_ref (annotations);
	PpsDocument *document = PPS_DOCUMENT (annotations);
	guint64 id = _pps_document_objects_get_id (_pps_document_proxy_get_objects (document), G_OBJECT (annotation));
	guint result = PPS_ANNOTATION_OVER_MARKUP_UNKNOWN;
	if (!pps_dbus_document_call_over_annotation_markup_sync (_pps_document_proxy_get_session (document)->proxy, id, x, y,
	                                                         G_DBUS_CALL_FLAGS_NONE, -1, &result, NULL, NULL) ||
	    result > PPS_ANNOTATION_OVER_MARKUP_NOT)
		return PPS_ANNOTATION_OVER_MARKUP_UNKNOWN;
	return result;
}

void
_pps_document_proxy_annotations_iface_init (PpsDocumentAnnotationsInterface *iface, gpointer data)
{
	guint methods = GPOINTER_TO_UINT (data);
	iface->get_annotations = get_annotations;
	iface->document_is_modified = is_modified;
	if (methods & PPS_ANNOTATION_METHOD_ADD)
		iface->add_annotation = add_annotation;
	if (methods & PPS_ANNOTATION_METHOD_REMOVE)
		iface->remove_annotation = remove_annotation;
	if (methods & PPS_ANNOTATION_METHOD_OVER_MARKUP)
		iface->over_markup = over_markup;
}

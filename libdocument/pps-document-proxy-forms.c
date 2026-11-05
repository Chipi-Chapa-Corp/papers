// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-form-private.h"
#include "pps-document-mapping-private.h"
#include "pps-document-proxy-private.h"

static PpsDBusDocument *
get_proxy (PpsDocumentForms *document)
{
	return _pps_document_proxy_get_session (PPS_DOCUMENT (document))->proxy;
}

static guint64
field_id (PpsDocumentForms *document, PpsFormField *field)
{
	return _pps_document_objects_get_id (_pps_document_proxy_get_objects (PPS_DOCUMENT (document)), G_OBJECT (field));
}

static GObject *
decode_field (GVariant *value, gpointer data, GError **error)
{
	return G_OBJECT (_pps_form_field_from_variant (value, data, error));
}

static PpsMappingList *
get_fields (PpsDocumentForms *document, PpsPage *page)
{
	g_autoptr (PpsDocumentForms) hold = g_object_ref (document);
	g_autoptr (GVariant) mappings = NULL;
	g_autoptr (GError) error = NULL;
	gboolean present;
	if (!pps_dbus_document_call_get_form_fields_sync (get_proxy (document), page->index,
	                                                  G_DBUS_CALL_FLAGS_NONE, -1, &present, &mappings, NULL, &error) ||
	    !present)
		return NULL;
	return _pps_mapping_list_from_variant (mappings, page->index, decode_field,
	                                       _pps_document_proxy_get_objects (PPS_DOCUMENT (document)), &error);
}

static gboolean
is_modified (PpsDocumentForms *document)
{
	return (_pps_document_proxy_get_state (PPS_DOCUMENT (document)) & PPS_DOCUMENT_STATE_FORMS_MODIFIED) != 0;
}

static char *
get_text (PpsDocumentForms *document, PpsFormField *field)
{
	g_autoptr (PpsDocumentForms) hold = g_object_ref (document);
	g_autofree char *text = NULL;
	gboolean present;
	if (!pps_dbus_document_call_get_form_text_sync (get_proxy (document), field_id (document, field),
	                                                G_DBUS_CALL_FLAGS_NONE, -1, &present, &text, NULL, NULL) ||
	    !present)
		return NULL;
	return g_steal_pointer (&text);
}

static void
set_text (PpsDocumentForms *document, PpsFormField *field, const char *text)
{
	g_autoptr (PpsDocumentForms) hold = g_object_ref (document);
	guint state;
	if (pps_dbus_document_call_set_form_text_sync (get_proxy (document), field_id (document, field), text != NULL, text ? text : "",
	                                               G_DBUS_CALL_FLAGS_NONE, -1, &state, NULL, NULL))
		_pps_document_proxy_set_state (PPS_DOCUMENT (document), state);
}

static gboolean
get_button (PpsDocumentForms *document, PpsFormField *field)
{
	g_autoptr (PpsDocumentForms) hold = g_object_ref (document);
	gboolean state = FALSE;
	pps_dbus_document_call_get_form_button_state_sync (get_proxy (document), field_id (document, field),
	                                                   G_DBUS_CALL_FLAGS_NONE, -1, &state, NULL, NULL);
	return state;
}

static void
set_button (PpsDocumentForms *document, PpsFormField *field, gboolean active)
{
	g_autoptr (PpsDocumentForms) hold = g_object_ref (document);
	guint state;
	if (pps_dbus_document_call_set_form_button_state_sync (get_proxy (document), field_id (document, field), active,
	                                                       G_DBUS_CALL_FLAGS_NONE, -1, &state, NULL, NULL))
		_pps_document_proxy_set_state (PPS_DOCUMENT (document), state);
}

static int
get_choice_count (PpsDocumentForms *document, PpsFormField *field)
{
	g_autoptr (PpsDocumentForms) hold = g_object_ref (document);
	int count = -1;
	pps_dbus_document_call_get_form_choice_count_sync (get_proxy (document), field_id (document, field),
	                                                   G_DBUS_CALL_FLAGS_NONE, -1, &count, NULL, NULL);
	return count;
}

static char *
get_choice_item (PpsDocumentForms *document, PpsFormField *field, int index)
{
	g_autoptr (PpsDocumentForms) hold = g_object_ref (document);
	g_autofree char *text = NULL;
	gboolean present;
	if (!pps_dbus_document_call_get_form_choice_item_sync (get_proxy (document), field_id (document, field), index,
	                                                       G_DBUS_CALL_FLAGS_NONE, -1, &present, &text, NULL, NULL) ||
	    !present)
		return NULL;
	return g_steal_pointer (&text);
}

static gboolean
get_choice_selected (PpsDocumentForms *document, PpsFormField *field, int index)
{
	g_autoptr (PpsDocumentForms) hold = g_object_ref (document);
	gboolean selected = FALSE;
	pps_dbus_document_call_get_form_choice_selected_sync (get_proxy (document), field_id (document, field), index,
	                                                      G_DBUS_CALL_FLAGS_NONE, -1, &selected, NULL, NULL);
	return selected;
}

static void
select_choice (PpsDocumentForms *document, PpsFormField *field, int index, gboolean toggle)
{
	g_autoptr (PpsDocumentForms) hold = g_object_ref (document);
	guint state;
	if (pps_dbus_document_call_select_form_choice_sync (get_proxy (document), field_id (document, field), index, toggle,
	                                                    G_DBUS_CALL_FLAGS_NONE, -1, &state, NULL, NULL))
		_pps_document_proxy_set_state (PPS_DOCUMENT (document), state);
}

static void
select_item (PpsDocumentForms *document, PpsFormField *field, int index)
{
	select_choice (document, field, index, FALSE);
}

static void
toggle_item (PpsDocumentForms *document, PpsFormField *field, int index)
{
	select_choice (document, field, index, TRUE);
}

static void
unselect_choice (PpsDocumentForms *document, PpsFormField *field)
{
	g_autoptr (PpsDocumentForms) hold = g_object_ref (document);
	guint state;
	if (pps_dbus_document_call_unselect_form_choice_sync (get_proxy (document), field_id (document, field),
	                                                      G_DBUS_CALL_FLAGS_NONE, -1, &state, NULL, NULL))
		_pps_document_proxy_set_state (PPS_DOCUMENT (document), state);
}

static void
reset_form (PpsDocumentForms *document, PpsLinkAction *action)
{
	g_autoptr (PpsDocumentForms) hold = g_object_ref (document);
	g_autoptr (GPtrArray) fields = g_ptr_array_new ();
	guint state;
	for (GList *item = pps_link_action_get_reset_fields (action); item; item = item->next)
		g_ptr_array_add (fields, item->data);
	g_ptr_array_add (fields, NULL);
	if (pps_dbus_document_call_reset_form_sync (get_proxy (document), (const char *const *) fields->pdata,
	                                            pps_link_action_get_exclude_reset_fields (action),
	                                            G_DBUS_CALL_FLAGS_NONE, -1, &state, NULL, NULL))
		_pps_document_proxy_set_state (PPS_DOCUMENT (document), state);
}

void
_pps_document_proxy_forms_iface_init (PpsDocumentFormsInterface *iface)
{
	iface->get_form_fields = get_fields;
	iface->document_is_modified = is_modified;
	iface->form_field_text_get_text = get_text;
	iface->form_field_text_set_text = set_text;
	iface->form_field_button_get_state = get_button;
	iface->form_field_button_set_state = set_button;
	iface->form_field_choice_get_item = get_choice_item;
	iface->form_field_choice_get_n_items = get_choice_count;
	iface->form_field_choice_is_item_selected = get_choice_selected;
	iface->form_field_choice_select_item = select_item;
	iface->form_field_choice_toggle_item = toggle_item;
	iface->form_field_choice_unselect_all = unselect_choice;
	iface->form_field_choice_set_text = set_text;
	iface->form_field_choice_get_text = get_text;
	iface->reset_form = reset_form;
}

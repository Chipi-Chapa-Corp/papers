// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-form-private.h"
#include "pps-document-mapping-private.h"
#include "pps-document-worker-private.h"

static GVariant *
encode_field (GObject *object, gpointer data)
{
	return _pps_form_field_to_variant (PPS_FORM_FIELD (object), data);
}

static gboolean
get_fields (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, int index, Worker *worker)
{
	if (!pps_document_worker_check_page (worker, index, PPS_TYPE_DOCUMENT_FORMS, invocation))
		return TRUE;
	if (!worker->forms)
		worker->forms = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) pps_mapping_list_unref);
	PpsMappingList *mappings = g_hash_table_lookup (worker->forms, GINT_TO_POINTER (index));
	if (!mappings) {
		g_autoptr (PpsPage) page = pps_document_get_page (worker->document, index);
		mappings = pps_document_forms_get_form_fields (PPS_DOCUMENT_FORMS (worker->document), page);
		if (mappings)
			g_hash_table_insert (worker->forms, GINT_TO_POINTER (index), mappings);
	}
	pps_dbus_document_complete_get_form_fields (interface, invocation, mappings != NULL,
	                                            _pps_mapping_list_to_variant (mappings, encode_field, worker->objects));
	return TRUE;
}

static void
invalid_field (GDBusMethodInvocation *invocation)
{
	g_dbus_method_invocation_return_error_literal (
	    invocation, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid document form field operation");
}

static PpsFormField *
lookup_field (Worker *worker, guint64 id, GType type, GDBusMethodInvocation *invocation)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_FORMS, invocation))
		return NULL;
	PpsFormField *field = (PpsFormField *) _pps_document_objects_lookup (worker->objects, id, type);
	if (!field)
		invalid_field (invocation);
	return field;
}

static PpsFormField *
lookup_text (Worker *worker, guint64 id, GDBusMethodInvocation *invocation)
{
	PpsFormField *field = lookup_field (worker, id, PPS_TYPE_FORM_FIELD, invocation);
	if (field && !PPS_IS_FORM_FIELD_TEXT (field) &&
	    !(PPS_IS_FORM_FIELD_CHOICE (field) && PPS_FORM_FIELD_CHOICE (field)->is_editable)) {
		invalid_field (invocation);
		return NULL;
	}
	return field;
}

static gboolean
get_text (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, guint64 id, Worker *worker)
{
	PpsFormField *field = lookup_text (worker, id, invocation);
	if (!field)
		return TRUE;
	PpsDocumentForms *forms = PPS_DOCUMENT_FORMS (worker->document);
	g_autofree char *text = PPS_IS_FORM_FIELD_TEXT (field)
	                            ? pps_document_forms_form_field_text_get_text (forms, field)
	                            : pps_document_forms_form_field_choice_get_text (forms, field);
	pps_dbus_document_complete_get_form_text (interface, invocation, text != NULL, text ? text : "");
	return TRUE;
}

static gboolean
set_text (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, guint64 id, gboolean present, const char *text, Worker *worker)
{
	PpsFormField *field = lookup_text (worker, id, invocation);
	if (!field)
		return TRUE;
	PpsDocumentForms *forms = PPS_DOCUMENT_FORMS (worker->document);
	if (PPS_IS_FORM_FIELD_TEXT (field))
		pps_document_forms_form_field_text_set_text (forms, field, present ? text : NULL);
	else
		pps_document_forms_form_field_choice_set_text (forms, field, present ? text : NULL);
	pps_dbus_document_complete_set_form_text (interface, invocation, pps_document_worker_get_state (worker));
	return TRUE;
}

static gboolean
get_button (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, guint64 id, Worker *worker)
{
	PpsFormField *field = lookup_field (worker, id, PPS_TYPE_FORM_FIELD_BUTTON, invocation);
	if (field)
		pps_dbus_document_complete_get_form_button_state (interface, invocation,
		                                                  pps_document_forms_form_field_button_get_state (PPS_DOCUMENT_FORMS (worker->document), field));
	return TRUE;
}

static gboolean
set_button (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, guint64 id, gboolean state, Worker *worker)
{
	PpsFormField *field = lookup_field (worker, id, PPS_TYPE_FORM_FIELD_BUTTON, invocation);
	if (!field)
		return TRUE;
	pps_document_forms_form_field_button_set_state (PPS_DOCUMENT_FORMS (worker->document), field, state);
	pps_dbus_document_complete_set_form_button_state (interface, invocation, pps_document_worker_get_state (worker));
	return TRUE;
}

static gboolean
get_choice_count (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, guint64 id, Worker *worker)
{
	PpsFormField *field = lookup_field (worker, id, PPS_TYPE_FORM_FIELD_CHOICE, invocation);
	if (field)
		pps_dbus_document_complete_get_form_choice_count (interface, invocation,
		                                                  pps_document_forms_form_field_choice_get_n_items (PPS_DOCUMENT_FORMS (worker->document), field));
	return TRUE;
}

static PpsFormField *
lookup_choice_item (Worker *worker, guint64 id, int index, GDBusMethodInvocation *invocation)
{
	PpsFormField *field = lookup_field (worker, id, PPS_TYPE_FORM_FIELD_CHOICE, invocation);
	if (field && (index < 0 || index >= pps_document_forms_form_field_choice_get_n_items (PPS_DOCUMENT_FORMS (worker->document), field))) {
		invalid_field (invocation);
		return NULL;
	}
	return field;
}

static gboolean
get_choice_item (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, guint64 id, int index, Worker *worker)
{
	PpsFormField *field = lookup_choice_item (worker, id, index, invocation);
	if (!field)
		return TRUE;
	g_autofree char *text = pps_document_forms_form_field_choice_get_item (PPS_DOCUMENT_FORMS (worker->document), field, index);
	pps_dbus_document_complete_get_form_choice_item (interface, invocation, text != NULL, text ? text : "");
	return TRUE;
}

static gboolean
get_choice_selected (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, guint64 id, int index, Worker *worker)
{
	PpsFormField *field = lookup_choice_item (worker, id, index, invocation);
	if (field)
		pps_dbus_document_complete_get_form_choice_selected (interface, invocation,
		                                                     pps_document_forms_form_field_choice_is_item_selected (PPS_DOCUMENT_FORMS (worker->document), field, index));
	return TRUE;
}

static gboolean
select_choice (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, guint64 id, int index, gboolean toggle, Worker *worker)
{
	PpsFormField *field = lookup_choice_item (worker, id, index, invocation);
	if (!field)
		return TRUE;
	if (toggle)
		pps_document_forms_form_field_choice_toggle_item (PPS_DOCUMENT_FORMS (worker->document), field, index);
	else
		pps_document_forms_form_field_choice_select_item (PPS_DOCUMENT_FORMS (worker->document), field, index);
	pps_dbus_document_complete_select_form_choice (interface, invocation, pps_document_worker_get_state (worker));
	return TRUE;
}

static gboolean
unselect_choice (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, guint64 id, Worker *worker)
{
	PpsFormField *field = lookup_field (worker, id, PPS_TYPE_FORM_FIELD_CHOICE, invocation);
	if (!field)
		return TRUE;
	pps_document_forms_form_field_choice_unselect_all (PPS_DOCUMENT_FORMS (worker->document), field);
	pps_dbus_document_complete_unselect_form_choice (interface, invocation, pps_document_worker_get_state (worker));
	return TRUE;
}

static gboolean
reset_form (PpsDBusDocument *interface, GDBusMethodInvocation *invocation, const char *const *fields, gboolean exclude, Worker *worker)
{
	if (!pps_document_worker_check_document (worker, PPS_TYPE_DOCUMENT_FORMS, invocation))
		return TRUE;
	GList *names = NULL;
	for (guint i = 0; fields[i]; i++)
		names = g_list_prepend (names, g_strdup (fields[i]));
	g_autoptr (PpsLinkAction) action = pps_link_action_new_reset_form (g_list_reverse (names), exclude);
	pps_document_forms_reset_form (PPS_DOCUMENT_FORMS (worker->document), action);
	pps_dbus_document_complete_reset_form (interface, invocation, pps_document_worker_get_state (worker));
	return TRUE;
}

void
pps_document_worker_bind_forms (Worker *worker, PpsDBusDocument *interface)
{
	g_signal_connect (interface, "handle-get-form-fields", G_CALLBACK (get_fields), worker);
	g_signal_connect (interface, "handle-get-form-text", G_CALLBACK (get_text), worker);
	g_signal_connect (interface, "handle-set-form-text", G_CALLBACK (set_text), worker);
	g_signal_connect (interface, "handle-get-form-button-state", G_CALLBACK (get_button), worker);
	g_signal_connect (interface, "handle-set-form-button-state", G_CALLBACK (set_button), worker);
	g_signal_connect (interface, "handle-get-form-choice-count", G_CALLBACK (get_choice_count), worker);
	g_signal_connect (interface, "handle-get-form-choice-item", G_CALLBACK (get_choice_item), worker);
	g_signal_connect (interface, "handle-get-form-choice-selected", G_CALLBACK (get_choice_selected), worker);
	g_signal_connect (interface, "handle-select-form-choice", G_CALLBACK (select_choice), worker);
	g_signal_connect (interface, "handle-unselect-form-choice", G_CALLBACK (unselect_choice), worker);
	g_signal_connect (interface, "handle-reset-form", G_CALLBACK (reset_form), worker);
}

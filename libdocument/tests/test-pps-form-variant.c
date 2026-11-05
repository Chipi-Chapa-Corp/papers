// SPDX-License-Identifier: GPL-2.0-or-later

#include <math.h>

#include "pps-document-form-private.h"
#include "pps-document-link-private.h"

static void
roundtrip (void)
{
	g_autoptr (PpsDocumentObjects) worker = _pps_document_objects_new ();
	g_autoptr (PpsDocumentObjects) client = _pps_document_objects_new ();
	g_autoptr (GPtrArray) fields = g_ptr_array_new_with_free_func (g_object_unref);
	for (int type = 0; type <= PPS_FORM_FIELD_TEXT_FILE_SELECT; type++) {
		PpsFormField *field = pps_form_field_text_new (type, type);
		PpsFormFieldText *text = PPS_FORM_FIELD_TEXT (field);
		text->do_spell_check = TRUE;
		text->comb = TRUE;
		text->is_password = TRUE;
		text->do_scroll = type == PPS_FORM_FIELD_TEXT_MULTILINE;
		text->is_rich_text = type == PPS_FORM_FIELD_TEXT_FILE_SELECT;
		text->max_len = 42;
		text->text = type ? g_strdup (type == 1 ? "" : "Field α") : NULL;
		g_ptr_array_add (fields, field);
	}
	for (int type = 0; type <= PPS_FORM_FIELD_BUTTON_RADIO; type++) {
		PpsFormField *field = pps_form_field_button_new (10 + type, type);
		PPS_FORM_FIELD_BUTTON (field)->state = TRUE;
		g_ptr_array_add (fields, field);
	}
	for (int type = 0; type <= PPS_FORM_FIELD_CHOICE_LIST; type++) {
		PpsFormField *field = pps_form_field_choice_new (20 + type, type);
		PpsFormFieldChoice *choice = PPS_FORM_FIELD_CHOICE (field);
		choice->multi_select = choice->is_editable = choice->commit_on_sel_change = TRUE;
		choice->do_spell_check = type == PPS_FORM_FIELD_CHOICE_LIST;
		choice->text = g_strdup ("Choice α");
		choice->selected_items = g_list_append (NULL, GINT_TO_POINTER (0));
		choice->selected_items = g_list_append (choice->selected_items, GINT_TO_POINTER (3));
		g_ptr_array_add (fields, field);
	}
	g_ptr_array_add (fields, pps_form_field_signature_new (30));
	for (guint i = 0; i < fields->len; i++) {
		PpsFormField *field = fields->pdata[i];
		g_autoptr (PpsLinkAction) action = pps_link_action_new_external_uri ("https://example.org/form");
		field->page = pps_page_new (3);
		field->is_read_only = field->changed = TRUE;
		field->font_size = 12.5;
		field->activation_link = pps_link_new ("Submit", action);
		pps_form_field_set_alternate_name (field, g_strdup ("Accessible name"));
		g_autoptr (GVariant) value = g_variant_ref_sink (_pps_form_field_to_variant (field, worker));
		g_autoptr (GError) error = NULL;
		g_autoptr (PpsFormField) copy = _pps_form_field_from_variant (value, client, &error);
		g_assert_no_error (error);
		g_assert_cmpuint (G_OBJECT_TYPE (copy), ==, G_OBJECT_TYPE (field));
		g_assert_cmpint (copy->id, ==, field->id);
		g_assert_cmpint (copy->page->index, ==, 3);
		g_assert_null (copy->page->backend_page);
		g_assert_true (copy->is_read_only);
		g_assert_true (copy->changed);
		g_assert_cmpfloat (copy->font_size, ==, 12.5);
		g_assert_cmpstr (pps_form_field_get_alternate_name (copy), ==, "Accessible name");
		g_assert_cmpstr (pps_link_action_get_uri (pps_link_get_action (copy->activation_link)), ==, "https://example.org/form");
		if (PPS_IS_FORM_FIELD_TEXT (copy)) {
			PpsFormFieldText *text = PPS_FORM_FIELD_TEXT (copy);
			g_assert_cmpint (text->type, ==, PPS_FORM_FIELD_TEXT (field)->type);
			g_assert_true (text->do_spell_check);
			g_assert_true (text->comb);
			g_assert_true (text->is_password);
			g_assert_cmpint (!!text->do_scroll, ==, text->type == PPS_FORM_FIELD_TEXT_MULTILINE);
			g_assert_cmpint (!!text->is_rich_text, ==, text->type == PPS_FORM_FIELD_TEXT_FILE_SELECT);
			g_assert_cmpint (text->max_len, ==, 42);
			g_assert_cmpstr (text->text, ==, PPS_FORM_FIELD_TEXT (field)->text);
		} else if (PPS_IS_FORM_FIELD_BUTTON (copy)) {
			g_assert_cmpint (PPS_FORM_FIELD_BUTTON (copy)->type, ==, PPS_FORM_FIELD_BUTTON (field)->type);
			g_assert_true (PPS_FORM_FIELD_BUTTON (copy)->state);
		} else if (PPS_IS_FORM_FIELD_CHOICE (copy)) {
			PpsFormFieldChoice *choice = PPS_FORM_FIELD_CHOICE (copy);
			g_assert_cmpint (choice->type, ==, PPS_FORM_FIELD_CHOICE (field)->type);
			g_assert_true (choice->multi_select);
			g_assert_true (choice->is_editable);
			g_assert_true (choice->commit_on_sel_change);
			g_assert_cmpint (!!choice->do_spell_check, ==, choice->type == PPS_FORM_FIELD_CHOICE_LIST);
			g_assert_cmpstr (choice->text, ==, "Choice α");
			g_assert_cmpuint (g_list_length (choice->selected_items), ==, 2);
			g_assert_cmpint (GPOINTER_TO_INT (choice->selected_items->data), ==, 0);
			g_assert_cmpint (GPOINTER_TO_INT (choice->selected_items->next->data), ==, 3);
		}
	}
}

static void
identity_preserves_pending_edits (void)
{
	g_autoptr (PpsDocumentObjects) worker = _pps_document_objects_new ();
	g_autoptr (PpsDocumentObjects) client = _pps_document_objects_new ();
	g_autoptr (PpsFormField) field = pps_form_field_text_new (1, PPS_FORM_FIELD_TEXT_NORMAL);
	g_autoptr (GVariant) value = g_variant_ref_sink (_pps_form_field_to_variant (field, worker));
	g_autoptr (GError) error = NULL;
	g_autoptr (PpsFormField) first = _pps_form_field_from_variant (value, client, &error);
	g_assert_no_error (error);
	g_assert_null (first->page);
	g_assert_null (first->activation_link);
	g_assert_null (pps_form_field_get_alternate_name (first));
	first->changed = TRUE;
	PPS_FORM_FIELD_TEXT (first)->text = g_strdup ("Not yet saved");
	g_autoptr (PpsFormField) again = _pps_form_field_from_variant (value, client, &error);
	g_assert_no_error (error);
	g_assert_true (first != again);
	g_assert_true (first->changed);
	g_assert_false (again->changed);
	g_assert_cmpstr (PPS_FORM_FIELD_TEXT (first)->text, ==, "Not yet saved");
	g_assert_null (PPS_FORM_FIELD_TEXT (again)->text);
	g_assert_cmpuint (_pps_document_objects_get_id (client, G_OBJECT (first)), ==,
	                  _pps_document_objects_get_id (client, G_OBJECT (again)));
}

static void
invalid_fields (void)
{
	g_autoptr (PpsDocumentObjects) worker = _pps_document_objects_new ();
	g_autoptr (PpsFormField) field = pps_form_field_text_new (1, PPS_FORM_FIELD_TEXT_NORMAL);
	g_autoptr (GVariant) valid = g_variant_ref_sink (_pps_form_field_to_variant (field, worker));
	const char *keys[] = { "id", "handle", "kind", "type", "page", "font-size", "text-flags", "text", "activation-link" };
	GVariant *values[] = { g_variant_new_int32 (-1), g_variant_new_uint64 (0), g_variant_new_uint32 (4),
		               g_variant_new_int32 (3), g_variant_new_int32 (-2), g_variant_new_double (NAN),
		               g_variant_new ("(bbbbbi)", FALSE, FALSE, FALSE, FALSE, FALSE, -1),
		               g_variant_new_int32 (1), g_variant_new_string ("invalid") };
	for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
		g_autoptr (PpsDocumentObjects) client = _pps_document_objects_new ();
		GVariantDict dictionary;
		g_variant_dict_init (&dictionary, valid);
		g_variant_dict_insert_value (&dictionary, keys[i], values[i]);
		g_autoptr (GVariant) value = g_variant_ref_sink (g_variant_dict_end (&dictionary));
		g_autoptr (GError) error = NULL;
		g_assert_null (_pps_form_field_from_variant (value, client, &error));
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	}
	g_autoptr (PpsDocumentObjects) client = _pps_document_objects_new ();
	g_autoptr (GObject) wrong = g_object_new (G_TYPE_OBJECT, NULL);
	g_assert_true (_pps_document_objects_import (client, 1, wrong));
	g_autoptr (GError) error = NULL;
	g_assert_null (_pps_form_field_from_variant (valid, client, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_clear_error (&error);
	g_autoptr (PpsFormField) choice = pps_form_field_choice_new (2, PPS_FORM_FIELD_CHOICE_LIST);
	g_autoptr (GVariant) choice_value = g_variant_ref_sink (_pps_form_field_to_variant (choice, worker));
	GVariantDict dictionary;
	g_variant_dict_init (&dictionary, choice_value);
	g_variant_dict_insert_value (&dictionary, "selected-items",
	                             g_variant_new_fixed_array (G_VARIANT_TYPE_INT32, (gint32[]){ 1, -1 }, 2, sizeof (gint32)));
	g_autoptr (GVariant) negative_item = g_variant_ref_sink (g_variant_dict_end (&dictionary));
	g_assert_null (_pps_form_field_from_variant (negative_item, client, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/document-form/roundtrip", roundtrip);
	g_test_add_func ("/document-form/pending-edits", identity_preserves_pending_edits);
	g_test_add_func ("/document-form/invalid", invalid_fields);
	return g_test_run ();
}

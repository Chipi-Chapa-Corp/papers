// SPDX-License-Identifier: GPL-2.0-or-later

#include <math.h>

#include "pps-document-form-private.h"
#include "pps-document-link-private.h"
#include "pps-document-variant-private.h"

GVariant *
_pps_form_field_to_variant (PpsFormField *field, PpsDocumentObjects *objects)
{
	GVariantBuilder builder;
	guint kind;
	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
	if (PPS_IS_FORM_FIELD_TEXT (field)) {
		PpsFormFieldText *text = PPS_FORM_FIELD_TEXT (field);
		kind = 0;
		g_variant_builder_add (&builder, "{sv}", "type", g_variant_new_int32 (text->type));
		g_variant_builder_add (&builder, "{sv}", "text-flags", g_variant_new ("(bbbbbi)", text->do_spell_check, text->do_scroll, text->comb, text->is_rich_text, text->is_password, text->max_len));
		_pps_variant_add_string (&builder, "text", text->text);
	} else if (PPS_IS_FORM_FIELD_BUTTON (field)) {
		PpsFormFieldButton *button = PPS_FORM_FIELD_BUTTON (field);
		kind = 1;
		g_variant_builder_add (&builder, "{sv}", "type", g_variant_new_int32 (button->type));
		g_variant_builder_add (&builder, "{sv}", "state", g_variant_new_boolean (button->state));
	} else if (PPS_IS_FORM_FIELD_CHOICE (field)) {
		PpsFormFieldChoice *choice = PPS_FORM_FIELD_CHOICE (field);
		GVariantBuilder selected;
		kind = 2;
		g_variant_builder_add (&builder, "{sv}", "type", g_variant_new_int32 (choice->type));
		g_variant_builder_add (&builder, "{sv}", "choice-flags", g_variant_new ("(bbbb)", choice->multi_select, choice->is_editable, choice->do_spell_check, choice->commit_on_sel_change));
		g_variant_builder_init (&selected, G_VARIANT_TYPE ("ai"));
		for (GList *item = choice->selected_items; item; item = item->next)
			g_variant_builder_add (&selected, "i", GPOINTER_TO_INT (item->data));
		g_variant_builder_add (&builder, "{sv}", "selected-items", g_variant_builder_end (&selected));
		_pps_variant_add_string (&builder, "text", choice->text);
	} else {
		g_assert (PPS_IS_FORM_FIELD_SIGNATURE (field));
		kind = 3;
	}
	g_variant_builder_add (&builder, "{sv}", "kind", g_variant_new_uint32 (kind));
	g_variant_builder_add (&builder, "{sv}", "handle", g_variant_new_uint64 (_pps_document_objects_export (objects, G_OBJECT (field))));
	g_variant_builder_add (&builder, "{sv}", "id", g_variant_new_int32 (field->id));
	g_variant_builder_add (&builder, "{sv}", "page", g_variant_new_int32 (field->page ? field->page->index : -1));
	g_variant_builder_add (&builder, "{sv}", "read-only", g_variant_new_boolean (field->is_read_only));
	g_variant_builder_add (&builder, "{sv}", "font-size", g_variant_new_double (field->font_size));
	g_variant_builder_add (&builder, "{sv}", "changed", g_variant_new_boolean (field->changed));
	g_variant_builder_add (&builder, "{sv}", "activation-link", _pps_link_to_variant (field->activation_link, objects));
	_pps_variant_add_string (&builder, "alternate-name", pps_form_field_get_alternate_name (field));
	return g_variant_builder_end (&builder);
}

PpsFormField *
_pps_form_field_from_variant (GVariant *value, PpsDocumentObjects *objects, GError **error)
{
	guint kind;
	guint64 handle;
	int id, page, type;
	gboolean read_only, changed;
	double font_size;
	g_autofree char *alternate = NULL;
	g_autoptr (GVariant) link_value = NULL;
	g_autoptr (PpsLink) link = NULL;
	g_autoptr (PpsFormField) field = NULL;
	if (!g_variant_is_of_type (value, G_VARIANT_TYPE_VARDICT) ||
	    !g_variant_lookup (value, "kind", "u", &kind) || kind > 3 ||
	    !g_variant_lookup (value, "handle", "t", &handle) || !handle ||
	    !g_variant_lookup (value, "id", "i", &id) || id < 0 ||
	    !g_variant_lookup (value, "page", "i", &page) || page < -1 ||
	    !g_variant_lookup (value, "read-only", "b", &read_only) ||
	    !g_variant_lookup (value, "font-size", "d", &font_size) || !isfinite (font_size) ||
	    !g_variant_lookup (value, "changed", "b", &changed) ||
	    !_pps_variant_read_string (value, "alternate-name", &alternate))
		goto invalid;
	link_value = g_variant_lookup_value (value, "activation-link", G_VARIANT_TYPE_VARDICT);
	if (!link_value)
		goto invalid;
	link = _pps_link_from_variant (link_value, objects, error);
	if (!link && g_variant_n_children (link_value))
		goto invalid;
	if (kind != 3 && (!g_variant_lookup (value, "type", "i", &type) || type < 0))
		goto invalid;
	switch (kind) {
	case 0: {
		gboolean spell, scroll, comb, rich, password;
		int max_len;
		if (type > PPS_FORM_FIELD_TEXT_FILE_SELECT ||
		    !g_variant_lookup (value, "text-flags", "(bbbbbi)", &spell, &scroll, &comb, &rich, &password, &max_len) || max_len < 0)
			goto invalid;
		field = pps_form_field_text_new (id, type);
		PpsFormFieldText *text = PPS_FORM_FIELD_TEXT (field);
		text->do_spell_check = spell;
		text->do_scroll = scroll;
		text->comb = comb;
		text->is_rich_text = rich;
		text->is_password = password;
		text->max_len = max_len;
		if (!_pps_variant_read_string (value, "text", &text->text))
			goto invalid;
		break;
	}
	case 1: {
		gboolean state;
		if (type > PPS_FORM_FIELD_BUTTON_RADIO || !g_variant_lookup (value, "state", "b", &state))
			goto invalid;
		field = pps_form_field_button_new (id, type);
		PPS_FORM_FIELD_BUTTON (field)->state = state;
		break;
	}
	case 2: {
		gboolean multi, editable, spell, commit;
		g_autoptr (GVariant) selected = g_variant_lookup_value (value, "selected-items", G_VARIANT_TYPE ("ai"));
		if (type > PPS_FORM_FIELD_CHOICE_LIST || !selected ||
		    !g_variant_lookup (value, "choice-flags", "(bbbb)", &multi, &editable, &spell, &commit))
			goto invalid;
		field = pps_form_field_choice_new (id, type);
		PpsFormFieldChoice *choice = PPS_FORM_FIELD_CHOICE (field);
		choice->multi_select = multi;
		choice->is_editable = editable;
		choice->do_spell_check = spell;
		choice->commit_on_sel_change = commit;
		for (gsize i = 0; i < g_variant_n_children (selected); i++) {
			int index;
			g_variant_get_child (selected, i, "i", &index);
			if (index < 0)
				goto invalid;
			choice->selected_items = g_list_prepend (choice->selected_items, GINT_TO_POINTER (index));
		}
		choice->selected_items = g_list_reverse (choice->selected_items);
		if (!_pps_variant_read_string (value, "text", &choice->text))
			goto invalid;
		break;
	}
	case 3:
		field = pps_form_field_signature_new (id);
	}
	field->page = page >= 0 ? pps_page_new (page) : NULL;
	field->is_read_only = read_only;
	field->font_size = font_size;
	field->changed = changed;
	field->activation_link = g_steal_pointer (&link);
	pps_form_field_set_alternate_name (field, g_steal_pointer (&alternate));
	/* Re-reading mappings must not replace a field being edited by an overlay. */
	GObject *existing = _pps_document_objects_lookup (objects, handle, G_OBJECT_TYPE (field));
	if (existing) {
		PpsFormField *previous = PPS_FORM_FIELD (existing);
		if (previous->id != id || (previous->page ? previous->page->index : -1) != page)
			goto invalid;
		return g_object_ref (previous);
	}
	if (!_pps_document_objects_import (objects, handle, G_OBJECT (field))) {
		existing = _pps_document_objects_lookup (objects, handle, G_OBJECT_TYPE (field));
		if (existing && PPS_FORM_FIELD (existing)->id == id &&
		    (PPS_FORM_FIELD (existing)->page ? PPS_FORM_FIELD (existing)->page->index : -1) == page)
			return g_object_ref (PPS_FORM_FIELD (existing));
		goto invalid;
	}
	return g_steal_pointer (&field);
invalid:
	if (!error || !*error)
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid document form field");
	return NULL;
}

// SPDX-License-Identifier: GPL-2.0-or-later

#include <gio/gio.h>

#include "pps-document-info-private.h"

static void
roundtrip (void)
{
	g_autoptr (GDateTime) created = g_date_time_new_from_iso8601 ("2026-11-05T12:34:56.123456+03:00", NULL);
	g_autoptr (GDateTime) modified = g_date_time_new_from_iso8601 ("2026-11-05T13:45:00Z", NULL);
	PpsDocumentPageSize sizes[] = { { 210, 297 }, { 297, 420 } };
	PpsDocumentLicense license = { "License text", "https://example.org/license", NULL };
	PpsDocumentInfo original = {
		.title = "Document α", .format = "PDF 1.7", .author = "Author", .subject = "Subject", .keywords = "Keywords", .creator = "Creator", .producer = "Producer", .linearized = "Yes", .security = "Encrypted", .creation_datetime = created, .modified_datetime = modified, .layout = PPS_DOCUMENT_LAYOUT_TWO_COLUMN_RIGHT, .mode = PPS_DOCUMENT_MODE_USE_ATTACHMENTS, .ui_hints = PPS_DOCUMENT_UI_HINT_DIRECTION_RTL, .permissions = PPS_DOCUMENT_PERMISSIONS_OK_TO_COPY, .n_pages = 2, .page_sizes = sizes, .license = &license, .contains_js = PPS_DOCUMENT_CONTAINS_JS_NO, .fields_mask = (PPS_DOCUMENT_INFO_CONTAINS_JS << 1) - 1
	};
	g_autoptr (GVariant) value = g_variant_ref_sink (_pps_document_info_to_variant (&original));
	g_autoptr (GError) error = NULL;
	g_autoptr (PpsDocumentInfo) decoded = _pps_document_info_from_variant (value, &error);

	g_assert_no_error (error);
	g_assert_nonnull (decoded);
	g_assert_cmpstr (decoded->title, ==, original.title);
	g_assert_cmpstr (decoded->license->text, ==, license.text);
	g_assert_null (decoded->license->web_statement);
	g_assert_cmpint (g_date_time_compare (created, decoded->creation_datetime), ==, 0);
	g_assert_cmpint (g_date_time_compare (modified, decoded->modified_datetime), ==, 0);
	g_assert_cmpfloat (decoded->page_sizes[1].width, ==, sizes[1].width);
	g_autoptr (GVariant) encoded = g_variant_ref_sink (_pps_document_info_to_variant (decoded));
	g_assert_true (g_variant_equal (value, encoded));
	g_autoptr (PpsDocumentInfo) copy = pps_document_info_copy (decoded);
	g_autoptr (GVariant) encoded_copy = g_variant_ref_sink (_pps_document_info_to_variant (copy));
	g_assert_true (g_variant_equal (value, encoded_copy));
}

static void
empty_info (void)
{
	PpsDocumentInfo original = { 0 };
	g_autoptr (GVariant) value = g_variant_ref_sink (_pps_document_info_to_variant (&original));
	g_autoptr (GError) error = NULL;
	g_autoptr (PpsDocumentInfo) decoded = _pps_document_info_from_variant (value, &error);

	g_assert_no_error (error);
	g_assert_nonnull (decoded);
	g_assert_null (decoded->title);
	g_assert_null (decoded->license);
	g_assert_null (decoded->creation_datetime);
	g_assert_null (decoded->page_sizes);
	g_assert_cmpuint (decoded->fields_mask, ==, 0);
	g_autoptr (PpsDocumentInfo) copy = pps_document_info_copy (decoded);
	g_assert_null (copy->creation_datetime);
	g_assert_null (copy->modified_datetime);
}

static void
invalid_info (void)
{
	PpsDocumentInfo original = { .n_pages = 1 };
	g_autoptr (GVariant) value = g_variant_ref_sink (_pps_document_info_to_variant (&original));
	const char *keys[] = { "title", "created", "license", "page-sizes", "n-pages" };
	GVariant *invalid[] = {
		g_variant_new_int32 (1), g_variant_new_string ("bad date"),
		g_variant_new_string ("not a license"),
		g_variant_new_array (G_VARIANT_TYPE ("(dd)"), NULL, 0),
		g_variant_new_int32 (-1)
	};

	for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
		GVariantDict dictionary;
		g_variant_dict_init (&dictionary, value);
		g_variant_dict_insert_value (&dictionary, keys[i], invalid[i]);
		g_autoptr (GVariant) malformed = g_variant_ref_sink (g_variant_dict_end (&dictionary));
		g_autoptr (GError) error = NULL;
		g_autoptr (PpsDocumentInfo) decoded = _pps_document_info_from_variant (malformed, &error);
		g_assert_null (decoded);
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	}
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/document-info/roundtrip", roundtrip);
	g_test_add_func ("/document-info/empty", empty_info);
	g_test_add_func ("/document-info/invalid", invalid_info);
	return g_test_run ();
}

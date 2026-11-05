// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-attachment-private.h"

static void
roundtrip (void)
{
	g_autoptr (GDateTime) modified = g_date_time_new_from_iso8601 ("2024-03-24T11:22:33.125000+05:30", NULL);
	g_autoptr (GDateTime) created = g_date_time_new_from_iso8601 ("2020-01-02T03:04:05Z", NULL);
	g_autoptr (PpsAttachment) original = pps_attachment_new ("notes.txt", "Embedded α", modified, created, 4, g_memdup2 ("a\0b\xff", 4));
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	g_autoptr (GError) error = NULL;
	g_autoptr (GVariant) value = g_variant_ref_sink (_pps_attachment_to_variant (original, descriptors, &error));
	g_assert_no_error (error);
	g_autoptr (PpsAttachment) copy = _pps_attachment_from_variant (value, descriptors, &error);
	g_assert_no_error (error);
	g_assert_nonnull (copy);
	g_assert_cmpstr (pps_attachment_get_name (copy), ==, "notes.txt");
	g_assert_cmpstr (pps_attachment_get_description (copy), ==, "Embedded α");
	g_assert_cmpstr (pps_attachment_get_mime_type (copy), ==, pps_attachment_get_mime_type (original));
	g_assert_true (g_date_time_equal (modified, pps_attachment_get_modification_datetime (copy)));
	g_assert_cmpint (g_date_time_get_utc_offset (modified), ==, g_date_time_get_utc_offset (pps_attachment_get_modification_datetime (copy)));
	g_assert_true (g_date_time_equal (created, pps_attachment_get_creation_datetime (copy)));
	g_clear_object (&descriptors);
	g_clear_object (&original);
	g_autoptr (GBytes) bytes = _pps_attachment_get_data (copy);
	gsize size;
	const guint8 *data = g_bytes_get_data (bytes, &size);
	g_assert_cmpmem (data, size, "a\0b\xff", 4);
}

static void
empty_and_optional (void)
{
	g_autoptr (GError) error = NULL;
	g_autoptr (GVariant) absent = g_variant_ref_sink (_pps_attachment_to_variant (NULL, NULL, &error));
	g_assert_no_error (error);
	g_assert_null (_pps_attachment_from_variant (absent, NULL, &error));
	g_assert_no_error (error);
	g_autoptr (PpsAttachment) original = pps_attachment_new (NULL, NULL, NULL, NULL, 0, NULL);
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	g_autoptr (GVariant) value = g_variant_ref_sink (_pps_attachment_to_variant (original, descriptors, &error));
	g_assert_no_error (error);
	g_autoptr (PpsAttachment) copy = _pps_attachment_from_variant (value, descriptors, &error);
	g_assert_no_error (error);
	g_assert_nonnull (copy);
	g_assert_null (pps_attachment_get_name (copy));
	g_assert_null (pps_attachment_get_description (copy));
	g_assert_null (pps_attachment_get_creation_datetime (copy));
	g_assert_null (pps_attachment_get_modification_datetime (copy));
	g_autoptr (GBytes) bytes = _pps_attachment_get_data (copy);
	g_assert_cmpuint (g_bytes_get_size (bytes), ==, 0);
}

static void
payload_lifetime (void)
{
	PpsAttachment *attachment = pps_attachment_new ("data", NULL, NULL, NULL, 4, g_strdup ("data"));
	GWeakRef weak;
	g_weak_ref_init (&weak, attachment);
	g_autoptr (GBytes) bytes = _pps_attachment_get_data (attachment);
	g_object_unref (attachment);
	g_autoptr (PpsAttachment) retained = g_weak_ref_get (&weak);
	g_assert_nonnull (retained);
	g_clear_object (&retained);
	g_assert_cmpmem (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes), "data", 4);
	g_clear_pointer (&bytes, g_bytes_unref);
	g_assert_null (g_weak_ref_get (&weak));
	g_weak_ref_clear (&weak);
}

static void
invalid_metadata (void)
{
	g_autoptr (PpsAttachment) attachment = pps_attachment_new ("data", NULL, NULL, NULL, 4, g_strdup ("data"));
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	g_autoptr (GError) error = NULL;
	g_autoptr (GVariant) valid = g_variant_ref_sink (_pps_attachment_to_variant (attachment, descriptors, &error));
	g_assert_no_error (error);
	const char *keys[] = { "name", "description", "created", "modified", "buffer", "buffer", "buffer", "buffer" };
	GVariant *values[] = { g_variant_new_int32 (1), g_variant_new_boolean (TRUE), g_variant_new_string ("invalid"),
		               g_variant_new_int32 (1), g_variant_new ("(ht)", -1, (guint64) 4),
		               g_variant_new ("(ht)", 1, (guint64) 4), g_variant_new ("(ht)", 0, (guint64) 5),
		               g_variant_new ("(ht)", 0, G_MAXUINT64) };
	for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
		GVariantDict dictionary;
		g_variant_dict_init (&dictionary, valid);
		g_variant_dict_insert_value (&dictionary, keys[i], values[i]);
		g_autoptr (GVariant) value = g_variant_ref_sink (g_variant_dict_end (&dictionary));
		g_assert_null (_pps_attachment_from_variant (value, descriptors, &error));
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
		g_clear_error (&error);
	}
	g_assert_null (_pps_attachment_from_variant (valid, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/document-attachment/roundtrip", roundtrip);
	g_test_add_func ("/document-attachment/empty-and-optional", empty_and_optional);
	g_test_add_func ("/document-attachment/payload-lifetime", payload_lifetime);
	g_test_add_func ("/document-attachment/invalid-metadata", invalid_metadata);
	return g_test_run ();
}

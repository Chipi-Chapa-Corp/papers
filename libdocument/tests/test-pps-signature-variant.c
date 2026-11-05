// SPDX-License-Identifier: GPL-2.0-or-later

#include <math.h>

#include "pps-document-signature-private.h"

static void
certificate_roundtrip (void)
{
	g_autoptr (GDateTime) issuance = g_date_time_new_from_iso8601 ("2024-03-27T12:34:56.123456+05:30", NULL);
	g_autoptr (GDateTime) expiration = g_date_time_new_from_iso8601 ("2034-03-27T12:34:56-03:00", NULL);
	for (int status = PPS_CERTIFICATE_STATUS_TRUSTED; status <= PPS_CERTIFICATE_STATUS_NOT_VERIFIED; status++) {
		g_autoptr (PpsCertificateInfo) certificate = g_object_new (PPS_TYPE_CERTIFICATE_INFO,
		                                                           "id", "certificate-id", "subject-common-name", "Signer α",
		                                                           "subject-email", "signer@example.org", "subject-organization", "",
		                                                           "issuer-common-name", "Issuer", "issuer-email", "ca@example.org",
		                                                           "issuer-organization", "Certificate Authority",
		                                                           "issuance-time", issuance, "expiration-time", expiration, "status", status, NULL);
		g_autoptr (GVariant) value = g_variant_ref_sink (_pps_certificate_info_to_variant (certificate));
		g_autoptr (GError) error = NULL;
		g_autoptr (PpsCertificateInfo) copy = _pps_certificate_info_from_variant (value, &error);
		g_assert_no_error (error);
		g_assert_nonnull (copy);
		g_clear_object (&certificate);
		g_autoptr (GVariant) encoded = g_variant_ref_sink (_pps_certificate_info_to_variant (copy));
		g_assert_true (g_variant_equal (value, encoded));
		g_autofree char *name = NULL;
		g_autoptr (GDateTime) time = NULL;
		g_object_get (copy, "subject-common-name", &name, "issuance-time", &time, NULL);
		g_assert_cmpstr (name, ==, "Signer α");
		g_assert_true (g_date_time_equal (issuance, time));
		g_assert_cmpint (g_date_time_get_microsecond (time), ==, 123456);
	}
}

static void
signature_roundtrip (void)
{
	g_autoptr (GDateTime) time = g_date_time_new_from_iso8601 ("2024-03-27T12:34:56.123456+05:30", NULL);
	g_autoptr (PpsCertificateInfo) certificate = g_object_new (PPS_TYPE_CERTIFICATE_INFO, "subject-common-name", "Signer", "status", PPS_CERTIFICATE_STATUS_TRUSTED, NULL);
	for (int status = PPS_SIGNATURE_STATUS_VALID; status <= PPS_SIGNATURE_STATUS_GENERIC_ERROR; status++) {
		g_autoptr (PpsSignature) signature = g_object_new (PPS_TYPE_SIGNATURE, "certificate-info", certificate,
		                                                   "signature-time", time, "status", status, NULL);
		g_autoptr (GVariant) value = g_variant_ref_sink (_pps_signature_info_to_variant (signature));
		g_autoptr (GError) error = NULL;
		g_autoptr (PpsSignature) copy = _pps_signature_info_from_variant (value, &error);
		g_assert_no_error (error);
		g_assert_nonnull (copy);
		g_assert_cmpint (pps_signature_is_valid (copy), ==, status == PPS_SIGNATURE_STATUS_VALID);
		g_clear_object (&signature);
		g_autoptr (GVariant) encoded = g_variant_ref_sink (_pps_signature_info_to_variant (copy));
		g_assert_true (g_variant_equal (value, encoded));
	}
}

static void
nullable_properties (void)
{
	g_autoptr (PpsCertificateInfo) certificate = g_object_new (PPS_TYPE_CERTIFICATE_INFO, "issuer-organization", "Issuer", NULL);
	g_autoptr (GVariant) info = g_variant_ref_sink (_pps_certificate_info_to_variant (certificate));
	g_autoptr (GError) error = NULL;
	g_autoptr (PpsCertificateInfo) copy = _pps_certificate_info_from_variant (info, &error);
	g_assert_no_error (error);
	g_assert_nonnull (copy);
	g_autoptr (GVariant) encoded = g_variant_ref_sink (_pps_certificate_info_to_variant (copy));
	g_assert_true (g_variant_equal (info, encoded));
	g_autoptr (PpsSignature) signature = g_object_new (PPS_TYPE_SIGNATURE, NULL);
	g_autoptr (GVariant) value = g_variant_ref_sink (_pps_signature_info_to_variant (signature));
	g_autoptr (PpsSignature) decoded = _pps_signature_info_from_variant (value, &error);
	g_assert_no_error (error);
	g_assert_nonnull (decoded);
	g_clear_object (&signature);
	g_autoptr (GVariant) again = g_variant_ref_sink (_pps_signature_info_to_variant (decoded));
	g_assert_true (g_variant_equal (value, again));
}

static void
invalid_information (void)
{
	g_autoptr (PpsCertificateInfo) certificate = g_object_new (PPS_TYPE_CERTIFICATE_INFO, "subject-common-name", "Signer", NULL);
	g_autoptr (PpsSignature) signature = pps_signature_new (PPS_SIGNATURE_STATUS_VALID, certificate);
	g_autoptr (GVariant) info = g_variant_ref_sink (_pps_certificate_info_to_variant (certificate));
	g_autoptr (GVariant) value = g_variant_ref_sink (_pps_signature_info_to_variant (signature));
	const struct {
		gboolean signature;
		const char *key;
		GVariant *invalid;
	} cases[] = {
		{ FALSE, "status", g_variant_new_int32 (-1) },
		{ FALSE, "status", g_variant_new_int32 (PPS_CERTIFICATE_STATUS_NOT_VERIFIED + 1) },
		{ FALSE, "subject-common-name", g_variant_new_int32 (1) },
		{ FALSE, "issuance-time", g_variant_new_string ("invalid date") },
		{ FALSE, "expiration-time", g_variant_new_boolean (FALSE) },
		{ TRUE, "status", g_variant_new_int32 (-1) },
		{ TRUE, "status", g_variant_new_int32 (PPS_SIGNATURE_STATUS_GENERIC_ERROR + 1) },
		{ TRUE, "signature-time", g_variant_new_string ("invalid date") },
		{ TRUE, "certificate-info", g_variant_new_string ("not a certificate") },
	};
	for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
		GVariantDict dict;
		g_variant_dict_init (&dict, cases[i].signature ? value : info);
		g_variant_dict_insert_value (&dict, cases[i].key, cases[i].invalid);
		g_autoptr (GVariant) malformed = g_variant_ref_sink (g_variant_dict_end (&dict));
		g_autoptr (GError) error = NULL;
		g_autoptr (GObject) decoded = cases[i].signature ? G_OBJECT (_pps_signature_info_from_variant (malformed, &error)) : G_OBJECT (_pps_certificate_info_from_variant (malformed, &error));
		g_assert_null (decoded);
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	}
	GVariant *wrong_types[] = { NULL, g_variant_new_string ("wrong type"), g_variant_new_array (G_VARIANT_TYPE ("{sv}"), NULL, 0) };
	for (guint i = 0; i < G_N_ELEMENTS (wrong_types); i++) {
		g_autoptr (GVariant) malformed = wrong_types[i] ? g_variant_ref_sink (wrong_types[i]) : NULL;
		g_autoptr (GError) error = NULL;
		g_assert_null (_pps_signature_info_from_variant (malformed, &error));
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
		g_clear_error (&error);
		g_assert_null (_pps_certificate_info_from_variant (malformed, &error));
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	}
}

static void
signing_data_roundtrip (void)
{
	g_autoptr (PpsCertificateInfo) certificate = g_object_new (PPS_TYPE_CERTIFICATE_INFO, "id", "signer", NULL);
	g_autoptr (PpsSignature) signature = pps_signature_new (PPS_SIGNATURE_STATUS_INVALID, certificate);
	PpsRectangle rect = { 10.5, 20.25, 200, 150 };
	GdkRGBA font = { .125, .25, .5, 1 }, border = { 1, 0, 0, .5 }, background = { 0, 1, 0, 0 };
	pps_signature_set_rect (signature, &rect);
	pps_signature_set_page (signature, 3);
	pps_signature_set_font_size (signature, 14);
	pps_signature_set_left_font_size (signature, 24);
	pps_signature_set_border_width (signature, 2);
	pps_signature_set_font_color (signature, &font);
	pps_signature_set_border_color (signature, &border);
	pps_signature_set_background_color (signature, &background);
	pps_signature_set_signature (signature, "Signed by α\nDate: 2024-03-27");
	pps_signature_set_signature_left (signature, "");
	pps_signature_set_password (signature, "certificate password");
	pps_signature_set_owner_password (signature, "owner password");
	pps_signature_set_user_password (signature, "user password");
	pps_signature_set_destination_file (signature, "/destination/stays/in/caller");
	g_autoptr (GVariant) value = g_variant_ref_sink (_pps_signature_signing_data_to_variant (signature));
	g_autoptr (GError) error = NULL;
	g_autoptr (PpsSignature) copy = _pps_signature_signing_data_from_variant (value, &error);
	g_assert_no_error (error);
	g_assert_nonnull (copy);
	g_clear_object (&signature);
	g_assert_cmpint (pps_signature_get_page (copy), ==, 3);
	g_assert_cmpint (pps_signature_get_font_size (copy), ==, 14);
	g_assert_cmpint (pps_signature_get_left_font_size (copy), ==, 24);
	g_assert_cmpint (pps_signature_get_border_width (copy), ==, 2);
	g_assert_cmpstr (pps_signature_get_signature (copy), ==, "Signed by α\nDate: 2024-03-27");
	g_assert_cmpstr (pps_signature_get_signature_left (copy), ==, "");
	g_assert_cmpstr (pps_signature_get_password (copy), ==, "certificate password");
	g_assert_cmpstr (pps_signature_get_owner_password (copy), ==, "owner password");
	g_assert_cmpstr (pps_signature_get_user_password (copy), ==, "user password");
	g_assert_null (pps_signature_get_destination_file (copy));
	g_assert_cmpfloat (pps_signature_get_rect (copy)->x1, ==, rect.x1);
	GdkRGBA color;
	pps_signature_get_font_color (copy, &color);
	g_assert_true (gdk_rgba_equal (&font, &color));
	pps_signature_get_border_color (copy, &color);
	g_assert_true (gdk_rgba_equal (&border, &color));
	pps_signature_get_background_color (copy, &color);
	g_assert_true (gdk_rgba_equal (&background, &color));
	g_autoptr (GVariant) encoded = g_variant_ref_sink (_pps_signature_signing_data_to_variant (copy));
	g_assert_true (g_variant_equal (value, encoded));
}

static void
invalid_signing_data (void)
{
	g_autoptr (PpsSignature) signature = g_object_new (PPS_TYPE_SIGNATURE, NULL);
	PpsRectangle rect = { 0, 0, 100, 100 };
	pps_signature_set_rect (signature, &rect);
	g_autoptr (GVariant) value = g_variant_ref_sink (_pps_signature_signing_data_to_variant (signature));
	g_autoptr (GError) error = NULL;
	g_autoptr (PpsSignature) copy = _pps_signature_signing_data_from_variant (value, &error);
	g_assert_no_error (error);
	g_assert_nonnull (copy);
	g_assert_null (pps_signature_get_password (copy));
	g_assert_null (pps_signature_get_signature (copy));
	g_assert_null (pps_signature_get_signature_left (copy));
	g_assert_null (pps_signature_get_owner_password (copy));
	g_assert_null (pps_signature_get_user_password (copy));
	const struct {
		const char *key;
		GVariant *invalid;
	} cases[] = {
		{ "page", g_variant_new_int32 (-1) },
		{ "page", g_variant_new_string ("0") },
		{ "font-size", g_variant_new_int32 (-1) },
		{ "left-font-size", g_variant_new_int32 (-1) },
		{ "border-width", g_variant_new_int32 (-1) },
		{ "rect", g_variant_new ("(dddd)", 0., 0., 0., 10.) },
		{ "rect", g_variant_new ("(dddd)", 0., 10., 10., 0.) },
		{ "rect", g_variant_new ("(dddd)", 0., 0., INFINITY, 10.) },
		{ "rect", g_variant_new_string ("rectangle") },
		{ "signature", g_variant_new_boolean (FALSE) },
		{ "signature-left", g_variant_new_int32 (0) },
		{ "password", g_variant_new_int32 (0) },
		{ "owner-password", g_variant_new_int32 (0) },
		{ "user-password", g_variant_new_int32 (0) },
		{ "font-color", g_variant_new ("(dddd)", NAN, 0., 0., 1.) },
		{ "border-color", g_variant_new ("(dddd)", -1., 0., 0., 1.) },
		{ "background-color", g_variant_new ("(dddd)", 0., 0., 0., 1.1) },
	};
	for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
		GVariantDict dict;
		g_variant_dict_init (&dict, value);
		g_variant_dict_insert_value (&dict, cases[i].key, cases[i].invalid);
		g_autoptr (GVariant) malformed = g_variant_ref_sink (g_variant_dict_end (&dict));
		g_assert_null (_pps_signature_signing_data_from_variant (malformed, &error));
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
		g_clear_error (&error);
	}
	const char *required[] = { "page", "rect", "font-size", "left-font-size", "border-width", "font-color", "border-color", "background-color" };
	for (guint i = 0; i < G_N_ELEMENTS (required); i++) {
		GVariantDict dict;
		g_variant_dict_init (&dict, value);
		g_variant_dict_remove (&dict, required[i]);
		g_autoptr (GVariant) malformed = g_variant_ref_sink (g_variant_dict_end (&dict));
		g_assert_null (_pps_signature_signing_data_from_variant (malformed, &error));
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
		g_clear_error (&error);
	}
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/document-signature/certificate-roundtrip", certificate_roundtrip);
	g_test_add_func ("/document-signature/signature-roundtrip", signature_roundtrip);
	g_test_add_func ("/document-signature/nullable-properties", nullable_properties);
	g_test_add_func ("/document-signature/invalid-information", invalid_information);
	g_test_add_func ("/document-signature/signing-data-roundtrip", signing_data_roundtrip);
	g_test_add_func ("/document-signature/invalid-signing-data", invalid_signing_data);
	return g_test_run ();
}

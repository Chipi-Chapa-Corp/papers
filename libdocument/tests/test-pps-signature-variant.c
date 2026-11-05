// SPDX-License-Identifier: GPL-2.0-or-later

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

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/document-signature/certificate-roundtrip", certificate_roundtrip);
	g_test_add_func ("/document-signature/signature-roundtrip", signature_roundtrip);
	g_test_add_func ("/document-signature/nullable-properties", nullable_properties);
	g_test_add_func ("/document-signature/invalid-information", invalid_information);
	return g_test_run ();
}

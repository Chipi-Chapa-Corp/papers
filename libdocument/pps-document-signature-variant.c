// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-signature-private.h"
#include "pps-document-variant-private.h"

static const char *certificate_strings[] = {
	"id", "subject-common-name", "subject-email", "subject-organization",
	"issuer-common-name", "issuer-email", "issuer-organization"
};

GVariant *
_pps_certificate_info_to_variant (PpsCertificateInfo *certificate)
{
	GVariantBuilder builder;
	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
	for (guint i = 0; i < G_N_ELEMENTS (certificate_strings); i++) {
		g_autofree char *text = NULL;
		g_object_get (certificate, certificate_strings[i], &text, NULL);
		_pps_variant_add_string (&builder, certificate_strings[i], text);
	}
	g_autoptr (GDateTime) issuance = NULL, expiration = NULL;
	int status;
	g_object_get (certificate, "issuance-time", &issuance, "expiration-time", &expiration, "status", &status, NULL);
	_pps_variant_add_date (&builder, "issuance-time", issuance);
	_pps_variant_add_date (&builder, "expiration-time", expiration);
	g_variant_builder_add (&builder, "{sv}", "status", g_variant_new_int32 (status));
	return g_variant_builder_end (&builder);
}

PpsCertificateInfo *
_pps_certificate_info_from_variant (GVariant *value, GError **error)
{
	g_autoptr (GPtrArray) strings = g_ptr_array_new_with_free_func (g_free);
	g_autoptr (GDateTime) issuance = NULL, expiration = NULL;
	int status;
	if (!value || !g_variant_is_of_type (value, G_VARIANT_TYPE_VARDICT) ||
	    !g_variant_lookup (value, "status", "i", &status) ||
	    status < PPS_CERTIFICATE_STATUS_TRUSTED || status > PPS_CERTIFICATE_STATUS_NOT_VERIFIED)
		goto invalid;
	for (guint i = 0; i < G_N_ELEMENTS (certificate_strings); i++) {
		char *text = NULL;
		if (!_pps_variant_read_string (value, certificate_strings[i], &text))
			goto invalid;
		g_ptr_array_add (strings, text);
	}
	if (!_pps_variant_read_date (value, "issuance-time", &issuance) ||
	    !_pps_variant_read_date (value, "expiration-time", &expiration))
		goto invalid;
	return g_object_new (PPS_TYPE_CERTIFICATE_INFO,
	                     "id", strings->pdata[0], "subject-common-name", strings->pdata[1],
	                     "subject-email", strings->pdata[2], "subject-organization", strings->pdata[3],
	                     "issuer-common-name", strings->pdata[4], "issuer-email", strings->pdata[5], "issuer-organization", strings->pdata[6],
	                     "issuance-time", issuance, "expiration-time", expiration, "status", status, NULL);
invalid:
	g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid document certificate information");
	return NULL;
}

GVariant *
_pps_signature_info_to_variant (PpsSignature *signature)
{
	GVariantBuilder builder;
	g_autoptr (PpsCertificateInfo) certificate = NULL;
	g_autoptr (GDateTime) time = NULL;
	int status;
	g_object_get (signature, "certificate-info", &certificate, "signature-time", &time, "status", &status, NULL);
	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add (&builder, "{sv}", "status", g_variant_new_int32 (status));
	_pps_variant_add_date (&builder, "signature-time", time);
	if (certificate)
		g_variant_builder_add (&builder, "{sv}", "certificate-info", _pps_certificate_info_to_variant (certificate));
	return g_variant_builder_end (&builder);
}

PpsSignature *
_pps_signature_info_from_variant (GVariant *value, GError **error)
{
	g_autoptr (PpsCertificateInfo) certificate = NULL;
	g_autoptr (GDateTime) time = NULL;
	g_autoptr (GVariant) info = NULL;
	int status;
	if (!value || !g_variant_is_of_type (value, G_VARIANT_TYPE_VARDICT) ||
	    !g_variant_lookup (value, "status", "i", &status) ||
	    status < PPS_SIGNATURE_STATUS_VALID || status > PPS_SIGNATURE_STATUS_GENERIC_ERROR ||
	    !_pps_variant_read_date (value, "signature-time", &time))
		goto invalid;
	info = g_variant_lookup_value (value, "certificate-info", NULL);
	if (info) {
		certificate = _pps_certificate_info_from_variant (info, error);
		if (!certificate)
			return NULL;
	}
	return g_object_new (PPS_TYPE_SIGNATURE, "certificate-info", certificate, "signature-time", time, "status", status, NULL);
invalid:
	g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid document signature information");
	return NULL;
}

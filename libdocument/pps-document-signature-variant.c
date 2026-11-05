// SPDX-License-Identifier: GPL-2.0-or-later

#include "pps-document-geometry-private.h"
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

GVariant *
_pps_signature_signing_data_to_variant (PpsSignature *signature)
{
	g_autoptr (GVariant) info = g_variant_ref_sink (_pps_signature_info_to_variant (signature));
	GVariantDict dict;
	g_variant_dict_init (&dict, info);
	g_variant_dict_insert (&dict, "page", "i", pps_signature_get_page (signature));
	g_variant_dict_insert (&dict, "font-size", "i", pps_signature_get_font_size (signature));
	g_variant_dict_insert (&dict, "left-font-size", "i", pps_signature_get_left_font_size (signature));
	g_variant_dict_insert (&dict, "border-width", "i", pps_signature_get_border_width (signature));
	PpsRectangle *rect = pps_signature_get_rect (signature);
	if (rect)
		g_variant_dict_insert (&dict, "rect", "(dddd)", rect->x1, rect->y1, rect->x2, rect->y2);
	const struct {
		const char *name;
		const char *value;
	} strings[] = {
		{ "signature", pps_signature_get_signature (signature) },
		{ "signature-left", pps_signature_get_signature_left (signature) },
		{ "password", pps_signature_get_password (signature) },
		{ "owner-password", pps_signature_get_owner_password (signature) },
		{ "user-password", pps_signature_get_user_password (signature) },
	};
	for (guint i = 0; i < G_N_ELEMENTS (strings); i++) {
		if (strings[i].value)
			g_variant_dict_insert (&dict, strings[i].name, "s", strings[i].value);
	}
	GdkRGBA color;
	pps_signature_get_font_color (signature, &color);
	g_variant_dict_insert_value (&dict, "font-color", _pps_rgba_to_variant (&color));
	pps_signature_get_border_color (signature, &color);
	g_variant_dict_insert_value (&dict, "border-color", _pps_rgba_to_variant (&color));
	pps_signature_get_background_color (signature, &color);
	g_variant_dict_insert_value (&dict, "background-color", _pps_rgba_to_variant (&color));
	return g_variant_dict_end (&dict);
}

PpsSignature *
_pps_signature_signing_data_from_variant (GVariant *value, GError **error)
{
	g_autoptr (PpsSignature) signature = _pps_signature_info_from_variant (value, error);
	g_autoptr (GVariant) rect_value = NULL;
	PpsRectangle rect;
	int page, font_size, left_font_size, border_width;
	if (!signature)
		return NULL;
	if (!g_variant_lookup (value, "page", "i", &page) || page < 0 ||
	    !g_variant_lookup (value, "font-size", "i", &font_size) || font_size < 0 ||
	    !g_variant_lookup (value, "left-font-size", "i", &left_font_size) || left_font_size < 0 ||
	    !g_variant_lookup (value, "border-width", "i", &border_width) || border_width < 0)
		goto invalid;
	rect_value = g_variant_lookup_value (value, "rect", NULL);
	if (!rect_value || !_pps_rectangle_from_variant (rect_value, &rect) || rect.x1 >= rect.x2 || rect.y1 >= rect.y2)
		goto invalid;
	pps_signature_set_page (signature, page);
	pps_signature_set_font_size (signature, font_size);
	pps_signature_set_left_font_size (signature, left_font_size);
	pps_signature_set_border_width (signature, border_width);
	pps_signature_set_rect (signature, &rect);
	const struct {
		const char *name;
		void (*set) (PpsSignature *, const char *);
	} strings[] = {
		{ "signature", pps_signature_set_signature },
		{ "signature-left", pps_signature_set_signature_left },
		{ "password", pps_signature_set_password },
		{ "owner-password", pps_signature_set_owner_password },
		{ "user-password", pps_signature_set_user_password },
	};
	for (guint i = 0; i < G_N_ELEMENTS (strings); i++) {
		g_autofree char *text = NULL;
		if (!_pps_variant_read_string (value, strings[i].name, &text))
			goto invalid;
		strings[i].set (signature, text);
	}
	const struct {
		const char *name;
		void (*set) (PpsSignature *, GdkRGBA *);
	} colors[] = {
		{ "font-color", pps_signature_set_font_color },
		{ "border-color", pps_signature_set_border_color },
		{ "background-color", pps_signature_set_background_color },
	};
	for (guint i = 0; i < G_N_ELEMENTS (colors); i++) {
		g_autoptr (GVariant) color_value = g_variant_lookup_value (value, colors[i].name, NULL);
		GdkRGBA color;
		if (!color_value || !_pps_rgba_from_variant (color_value, &color) ||
		    color.red < 0 || color.red > 1 || color.green < 0 || color.green > 1 ||
		    color.blue < 0 || color.blue > 1 || color.alpha < 0 || color.alpha > 1)
			goto invalid;
		colors[i].set (signature, &color);
	}
	return g_steal_pointer (&signature);
invalid:
	g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid document signing data");
	return NULL;
}

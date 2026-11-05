// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-signature.h"
#include <gio/gio.h>

PPS_PRIVATE
GVariant *_pps_certificate_info_to_variant (PpsCertificateInfo *certificate);
PPS_PRIVATE
PpsCertificateInfo *_pps_certificate_info_from_variant (GVariant *value, GError **error);
PPS_PRIVATE
GVariant *_pps_signature_info_to_variant (PpsSignature *signature);
PPS_PRIVATE
PpsSignature *_pps_signature_info_from_variant (GVariant *value, GError **error);

/* The destination is supplied separately as an output descriptor. */
PPS_PRIVATE
GVariant *_pps_signature_signing_data_to_variant (PpsSignature *signature);
PPS_PRIVATE
PpsSignature *_pps_signature_signing_data_from_variant (GVariant *value, GError **error);

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "pps-annotation.h"
#include <gio/gunixfdlist.h>

PPS_PRIVATE
GVariant *_pps_annotation_to_variant (PpsAnnotation *annotation, GUnixFDList *fd_list, GError **error);
/* A backend page can be supplied by the worker; NULL creates an index-only page. */
PPS_PRIVATE
PpsAnnotation *_pps_annotation_from_variant (GVariant *value, PpsPage *page, GUnixFDList *fd_list, GError **error);
PPS_PRIVATE
GVariant *_pps_annotation_property_to_variant (PpsAnnotation *annotation, const char *name, GUnixFDList *fd_list, GError **error);
PPS_PRIVATE
gboolean _pps_annotation_set_property_from_variant (PpsAnnotation *annotation, const char *name, GVariant *value, GUnixFDList *fd_list, GError **error);

// SPDX-License-Identifier: GPL-2.0-or-later
/* this file is part of papers, a gnome document viewer
 *
 *  Copyright (C) 2004 Martin Kretzschmar
 *
 *  Author:
 *    Martin Kretzschmar <martink@gnome.org>
 */

#include "pps-document.h"
#include "pps-file-exporter-private.h"
#include <config.h>

G_DEFINE_INTERFACE (PpsFileExporter, pps_file_exporter, 0)

static void
pps_file_exporter_default_init (PpsFileExporterInterface *klass)
{
}

void
pps_file_exporter_begin (PpsFileExporter *exporter,
                         PpsFileExporterContext *fc)
{
	PpsFileExporterInterface *iface = PPS_FILE_EXPORTER_GET_IFACE (exporter);

	_pps_file_exporter_take_error (exporter, NULL);
	iface->begin (exporter, fc);
}

void
pps_file_exporter_begin_page (PpsFileExporter *exporter)
{
	PpsFileExporterInterface *iface = PPS_FILE_EXPORTER_GET_IFACE (exporter);

	if (iface->begin_page)
		iface->begin_page (exporter);
}

void
pps_file_exporter_do_page (PpsFileExporter *exporter,
                           PpsRenderContext *rc)
{
	PpsFileExporterInterface *iface = PPS_FILE_EXPORTER_GET_IFACE (exporter);

	iface->do_page (exporter, rc);
}

void
pps_file_exporter_end_page (PpsFileExporter *exporter)
{
	PpsFileExporterInterface *iface = PPS_FILE_EXPORTER_GET_IFACE (exporter);

	if (iface->end_page)
		iface->end_page (exporter);
}

void
pps_file_exporter_end (PpsFileExporter *exporter)
{
	PpsFileExporterInterface *iface = PPS_FILE_EXPORTER_GET_IFACE (exporter);

	iface->end (exporter);
}

PpsFileExporterCapabilities
pps_file_exporter_get_capabilities (PpsFileExporter *exporter)
{
	PpsFileExporterInterface *iface = PPS_FILE_EXPORTER_GET_IFACE (exporter);

	return iface->get_capabilities (exporter);
}

static GQuark
export_error_quark (void)
{
	return g_quark_from_static_string ("pps-file-exporter-error");
}

void
_pps_file_exporter_take_error (PpsFileExporter *exporter, GError *error)
{
	GObject *object = G_OBJECT (exporter);
	if (!error)
		g_object_set_qdata_full (object, export_error_quark (), NULL, NULL);
	else if (!g_object_replace_qdata (object, export_error_quark (), NULL, error, (GDestroyNotify) g_error_free, NULL))
		g_error_free (error);
}

static gpointer
copy_error (gpointer error, gpointer data)
{
	return error ? g_error_copy (error) : NULL;
}

gboolean
_pps_file_exporter_get_error (PpsFileExporter *exporter, GError **error)
{
	if (!error)
		return g_object_get_qdata (G_OBJECT (exporter), export_error_quark ()) != NULL;
	GError *copy = g_object_dup_qdata (G_OBJECT (exporter), export_error_quark (), copy_error, NULL);
	if (!copy)
		return FALSE;
	g_propagate_error (error, copy);
	return TRUE;
}

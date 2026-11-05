// SPDX-License-Identifier: GPL-2.0-or-later

#include <cairo-pdf.h>
#include <glib/gstdio.h>

#include "pps-document-factory-private.h"
#include "pps-document-proxy-private.h"
#include "pps-file-exporter.h"
#include "pps-init.h"

static void
pdf_nup_unlocks_document (gconstpointer data)
{
	if (!g_test_subprocess ()) {
		g_test_trap_subprocess (NULL, 5 * G_USEC_PER_SEC, 0);
		g_test_trap_assert_passed ();
		return;
	}
	gboolean proxy = GPOINTER_TO_INT (data);
	g_autoptr (GError) error = NULL;
	g_autofree char *directory = g_dir_make_tmp ("papers-export-test-XXXXXX", &error);
	g_assert_no_error (error);
	g_autofree char *source_path = g_build_filename (directory, "source.pdf", NULL);
	g_autofree char *source_uri = g_filename_to_uri (source_path, NULL, NULL);
	cairo_surface_t *surface = cairo_pdf_surface_create (source_path, 100, 200);
	cairo_t *cr = cairo_create (surface);
	for (guint i = 0; i < 2; i++) {
		cairo_move_to (cr, 10, 30);
		cairo_show_text (cr, i ? "Page two" : "Page one");
		cairo_show_page (cr);
	}
	cairo_destroy (cr);
	cairo_surface_destroy (surface);
	g_autoptr (PpsDocument) document = proxy ? _pps_document_proxy_new ("application/pdf", NULL, &error) : _pps_document_factory_create_document ("application/pdf", &error);
	g_assert_no_error (error);
	g_assert_true (pps_document_load (document, source_uri, &error));
	g_assert_no_error (error);
	pps_document_setup_cache (document);
	g_autofree char *export_path = g_build_filename (directory, "nup.pdf", NULL);
	PpsFileExporterContext context = { PPS_FILE_FORMAT_PDF, export_path, 0, 1, 200, 200, FALSE, 2 };
	PpsFileExporter *exporter = PPS_FILE_EXPORTER (document);
	pps_file_exporter_begin (exporter, &context);
	pps_file_exporter_begin_page (exporter);
	for (int i = 0; i < 2; i++) {
		g_autoptr (PpsPage) page = pps_document_get_page (document, i);
		g_autoptr (PpsRenderContext) rc = pps_render_context_new (page, 0, 1, PPS_RENDER_ANNOTS_ALL);
		pps_file_exporter_do_page (exporter, rc);
	}
	pps_file_exporter_end_page (exporter);
	pps_file_exporter_end (exporter);
	/* Saving needs the writer lock after both rendered pages have finished. */
	g_autofree char *saved_path = g_build_filename (directory, "saved.pdf", NULL);
	g_autofree char *saved_uri = g_filename_to_uri (saved_path, NULL, NULL);
	g_assert_true (pps_document_save (document, saved_uri, &error));
	g_assert_no_error (error);
	g_assert_cmpint (g_unlink (saved_path), ==, 0);
	g_assert_cmpint (g_unlink (export_path), ==, 0);
	g_assert_cmpint (g_unlink (source_path), ==, 0);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_assert_true (pps_init ());
	g_test_add_data_func ("/document-export/native-nup-unlocks-document", GINT_TO_POINTER (FALSE), pdf_nup_unlocks_document);
	int result = g_test_run ();
	pps_shutdown ();
	return result;
}

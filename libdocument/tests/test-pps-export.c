// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <cairo-pdf.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pps-document-factory-private.h"
#include "pps-document-proxy-private.h"
#include "pps-file-exporter-private.h"
#include "pps-init.h"

static char *
create_pdf (const char *directory)
{
	char *path = g_build_filename (directory, "source.pdf", NULL);
	cairo_surface_t *surface = cairo_pdf_surface_create (path, 100, 200);
	cairo_t *cr = cairo_create (surface);
	for (guint i = 0; i < 2; i++) {
		cairo_move_to (cr, 10, 30);
		cairo_show_text (cr, i ? "Page two" : "Page one");
		cairo_show_page (cr);
	}
	cairo_destroy (cr);
	cairo_surface_destroy (surface);
	return path;
}

static PpsDocument *
open_document (const char *mime_type, const char *path, gboolean proxy)
{
	g_autoptr (GError) error = NULL;
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	PpsDocument *document = proxy ? _pps_document_proxy_new (mime_type, NULL, &error) : _pps_document_factory_create_document (mime_type, &error);
	g_assert_no_error (error);
	g_assert_nonnull (document);
	g_assert_true (pps_document_load (document, uri, &error));
	g_assert_no_error (error);
	pps_document_setup_cache (document);
	return document;
}

static void
export_page (PpsDocument *document, int index)
{
	g_autoptr (PpsPage) page = pps_document_get_page (document, index);
	g_autoptr (PpsRenderContext) rc = pps_render_context_new (page, 0, 1, PPS_RENDER_ANNOTS_ALL);
	pps_file_exporter_do_page (PPS_FILE_EXPORTER (document), rc);
}

static void
pdf_nup_unlocks_document (gconstpointer data)
{
	if (!g_test_subprocess ()) {
		g_test_trap_subprocess (NULL, 5 * G_USEC_PER_SEC, 0);
		g_test_trap_assert_passed ();
		return;
	}
	g_autoptr (GError) error = NULL;
	g_autofree char *directory = g_dir_make_tmp ("papers-export-test-XXXXXX", &error);
	g_assert_no_error (error);
	g_autofree char *source_path = create_pdf (directory);
	g_autoptr (PpsDocument) document = open_document ("application/pdf", source_path, GPOINTER_TO_INT (data));
	g_autofree char *export_path = g_build_filename (directory, "nup.pdf", NULL);
	PpsFileExporterContext context = { PPS_FILE_FORMAT_PDF, export_path, 0, 1, 200, 200, FALSE, 2 };
	PpsFileExporter *exporter = PPS_FILE_EXPORTER (document);
	pps_file_exporter_begin (exporter, &context);
	pps_file_exporter_begin_page (exporter);
	export_page (document, 0);
	export_page (document, 1);
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

static void
finalization_failure (gconstpointer data)
{
	g_autoptr (GError) error = NULL;
	g_autofree char *directory = g_dir_make_tmp ("papers-export-test-XXXXXX", &error);
	g_assert_no_error (error);
	g_autofree char *source = create_pdf (directory);
	g_autoptr (PpsDocument) document = open_document ("application/pdf", source, GPOINTER_TO_INT (data));
	int fd = memfd_create ("papers-export-failure", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	g_assert_cmpint (fd, >=, 0);
	/* Open succeeds, but the destination cannot grow when Cairo flushes it. */
	g_assert_cmpint (fcntl (fd, F_ADD_SEALS, F_SEAL_GROW), ==, 0);
	g_autofree char *path = g_strdup_printf ("/proc/self/fd/%d", fd);
	PpsFileExporterContext context = { PPS_FILE_FORMAT_PDF, path, 0, 1, 200, 200, FALSE, 1 };
	PpsFileExporter *exporter = PPS_FILE_EXPORTER (document);
	pps_file_exporter_begin (exporter, &context);
	pps_file_exporter_begin_page (exporter);
	export_page (document, 0);
	pps_file_exporter_end_page (exporter);
	pps_file_exporter_end (exporter);
	g_assert_true (_pps_file_exporter_get_error (exporter, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
	g_clear_error (&error);
	close (fd);
	g_autofree char *valid = g_build_filename (directory, "valid.pdf", NULL);
	context.filename = valid;
	pps_file_exporter_begin (exporter, &context);
	pps_file_exporter_begin_page (exporter);
	export_page (document, 1);
	pps_file_exporter_end_page (exporter);
	pps_file_exporter_end (exporter);
	g_assert_false (_pps_file_exporter_get_error (exporter, NULL));
	g_autoptr (PpsDocument) saved = open_document ("application/pdf", valid, FALSE);
	g_assert_cmpint (pps_document_get_n_pages (saved), ==, 1);
	g_assert_cmpint (g_unlink (valid), ==, 0);
	g_assert_cmpint (g_unlink (source), ==, 0);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_assert_true (pps_init ());
	g_test_add_data_func ("/document-export/native-nup-unlocks-document", GINT_TO_POINTER (FALSE), pdf_nup_unlocks_document);
	g_test_add_data_func ("/document-export/native-finalization-failure", GINT_TO_POINTER (FALSE), finalization_failure);
	int result = g_test_run ();
	pps_shutdown ();
	return result;
}

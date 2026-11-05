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

static guint
count_text (const char *text, const char *needle)
{
	guint count = 0;
	while ((text = strstr (text, needle))) {
		count++;
		text += strlen (needle);
	}
	return count;
}

static void
pdf_vector_export (gconstpointer data)
{
	g_autoptr (GError) error = NULL;
	g_autofree char *directory = g_dir_make_tmp ("papers-export-test-XXXXXX", &error);
	g_assert_no_error (error);
	g_autofree char *source_path = create_pdf (directory);
	g_autoptr (PpsDocument) document = open_document ("application/pdf", source_path, GPOINTER_TO_INT (data));
	PpsFileExporter *exporter = PPS_FILE_EXPORTER (document);
	PpsFileExporterCapabilities capabilities = pps_file_exporter_get_capabilities (exporter);
	g_assert_true ((capabilities & PPS_FILE_EXPORTER_CAN_GENERATE_PDF) != 0);
	g_assert_true ((capabilities & PPS_FILE_EXPORTER_CAN_GENERATE_PS) != 0);
	g_assert_true ((capabilities & PPS_FILE_EXPORTER_CAN_NUMBER_UP) != 0);
	g_autofree char *export_path = g_build_filename (directory, "export.pdf", NULL);
	const int nups[] = { 1, 2, 4, 6, 9, 16 };
	for (guint n = 0; n < G_N_ELEMENTS (nups); n++) {
		PpsFileExporterContext context = { PPS_FILE_FORMAT_PDF, export_path, 0, 1, 200, 200, TRUE, nups[n] };
		pps_file_exporter_begin (exporter, &context);
		int count = nups[n] == 1 ? 3 : nups[n];
		for (int i = 0; i < count; i++) {
			if (i % nups[n] == 0)
				pps_file_exporter_begin_page (exporter);
			/* Reordered pages and repeated copies must survive transport. */
			export_page (document, (i + 1) % 2);
			if ((i + 1) % nups[n] == 0)
				pps_file_exporter_end_page (exporter);
		}
		pps_file_exporter_end (exporter);
		g_autoptr (PpsDocument) exported = open_document ("application/pdf", export_path, FALSE);
		int sheets = nups[n] == 1 ? 3 : 1;
		g_assert_cmpint (pps_document_get_n_pages (exported), ==, sheets);
		g_autoptr (GString) all_text = g_string_new (NULL);
		for (int i = 0; i < sheets; i++) {
			double width, height;
			pps_document_get_page_size (exported, i, &width, &height);
			g_assert_cmpfloat (width, ==, 200);
			g_assert_cmpfloat (height, ==, 200);
			g_autoptr (PpsPage) page = pps_document_get_page (exported, i);
			g_autofree char *text = pps_document_text_get_text (PPS_DOCUMENT_TEXT (exported), page);
			g_assert_nonnull (text);
			if (nups[n] == 1)
				g_assert_nonnull (strstr (text, i == 1 ? "Page one" : "Page two"));
			g_string_append (all_text, text);
		}
		/* Selectable text proves this path has not flattened the pages. */
		g_assert_cmpuint (count_text (all_text->str, "Page one"), ==, count / 2);
		g_assert_cmpuint (count_text (all_text->str, "Page two"), ==, (count + 1) / 2);
	}
	g_autofree char *ps_path = g_build_filename (directory, "export.ps", NULL);
	PpsFileExporterContext ps_context = { PPS_FILE_FORMAT_PS, ps_path, 0, 1, 200, 200, FALSE, 1 };
	pps_file_exporter_begin (exporter, &ps_context);
	for (int i = 0; i < 3; i++) {
		pps_file_exporter_begin_page (exporter);
		export_page (document, i % 2);
		pps_file_exporter_end_page (exporter);
	}
	pps_file_exporter_end (exporter);
	g_autofree char *postscript = NULL;
	g_assert_true (g_file_get_contents (ps_path, &postscript, NULL, &error));
	g_assert_no_error (error);
	g_assert_true (g_str_has_prefix (postscript, "%!PS-Adobe"));
	g_assert_nonnull (strstr (postscript, "%%Pages: 3"));
	g_assert_cmpint (g_unlink (ps_path), ==, 0);
	g_assert_cmpint (g_unlink (export_path), ==, 0);
	g_assert_cmpint (g_unlink (source_path), ==, 0);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}

static void
legacy_postscript (gconstpointer data)
{
	guint options = GPOINTER_TO_UINT (data);
	gboolean proxy = options & 1, tiff = options & 2;
	if (tiff ? !TEST_HAVE_TIFF : !TEST_HAVE_DJVU) {
		g_test_skip ("The document backend is disabled");
		return;
	}
	g_autoptr (GError) error = NULL;
	g_autofree char *directory = g_dir_make_tmp ("papers-export-test-XXXXXX", &error);
	g_assert_no_error (error);
	g_autofree char *source = g_build_filename (directory, tiff ? "source.tiff" : "source.djvu", NULL);
	if (tiff) {
		g_autoptr (GdkPixbuf) pixels = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 3, 2);
		gdk_pixbuf_fill (pixels, 0xff0000ff);
		g_assert_true (gdk_pixbuf_save (pixels, source, "tiff", &error, NULL));
	} else {
		/* A blank DjVu page: FORM:DJVU containing a 2x2, 72dpi INFO chunk. */
		const char bytes[] = "AT&TFORM\000\000\000\026DJVUINFO\000\000\000\012\000\002\000\002\030\000\110\000\026\001";
		g_assert_true (g_file_set_contents (source, bytes, sizeof bytes - 1, &error));
	}
	g_assert_no_error (error);
	g_autoptr (PpsDocument) document = open_document (tiff ? "image/tiff" : "image/vnd.djvu", source, proxy);
	g_assert_cmpint (pps_document_get_n_pages (document), ==, 1);
	PpsFileExporter *exporter = PPS_FILE_EXPORTER (document);
	PpsFileExporterCapabilities capabilities = pps_file_exporter_get_capabilities (exporter);
	g_assert_true ((capabilities & PPS_FILE_EXPORTER_CAN_GENERATE_PS) != 0);
	g_assert_false ((capabilities & PPS_FILE_EXPORTER_CAN_GENERATE_PDF) != 0);
	g_autofree char *destination = g_build_filename (directory, "export.ps", NULL);
	PpsFileExporterContext context = { PPS_FILE_FORMAT_PS, destination, 0, 0, 200, 200, FALSE, 1 };
	/* Reusing an exporter exercises retained descriptors and native context reset. */
	for (int i = 0; i < 2; i++) {
		pps_file_exporter_begin (exporter, &context);
		pps_file_exporter_begin_page (exporter);
		export_page (document, 0);
		pps_file_exporter_end_page (exporter);
		pps_file_exporter_end (exporter);
		g_autofree char *postscript = NULL;
		g_assert_true (g_file_get_contents (destination, &postscript, NULL, &error));
		g_assert_no_error (error);
		g_assert_true (g_str_has_prefix (postscript, "%!PS-Adobe"));
	}
	g_assert_cmpint (g_unlink (destination), ==, 0);
	g_assert_cmpint (g_unlink (source), ==, 0);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}

static void
export_descriptor_and_errors (void)
{
	g_autoptr (GError) error = NULL;
	g_autofree char *directory = g_dir_make_tmp ("papers-export-test-XXXXXX", &error);
	g_assert_no_error (error);
	g_autofree char *source_path = create_pdf (directory);
	g_autoptr (PpsDocument) document = open_document ("application/pdf", source_path, TRUE);
	PpsDocumentSession *session = _pps_document_proxy_get_session (document);
	g_assert_false (pps_dbus_document_call_export_begin_page_sync (session->proxy, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
	g_clear_error (&error);
	g_autofree char *path = g_build_filename (directory, "unlinked.pdf", NULL);
	int fd = g_open (path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	g_assert_cmpint (fd, >=, 0);
	g_assert_cmpint (g_unlink (path), ==, 0);
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	int handle = g_unix_fd_list_append (descriptors, fd, &error);
	g_assert_no_error (error);
	const struct {
		int handle;
		guint format;
		int first, last;
		double width, height;
		int nup;
	} invalid[] = {
		{ -1, PPS_FILE_FORMAT_PDF, 0, 1, 200, 200, 1 },
		{ 1, PPS_FILE_FORMAT_PDF, 0, 1, 200, 200, 1 },
		{ 0, PPS_FILE_FORMAT_UNKNOWN, 0, 1, 200, 200, 1 },
		{ 0, PPS_FILE_FORMAT_PDF, -1, 1, 200, 200, 1 },
		{ 0, PPS_FILE_FORMAT_PDF, 1, 0, 200, 200, 1 },
		{ 0, PPS_FILE_FORMAT_PDF, 0, 2, 200, 200, 1 },
		{ 0, PPS_FILE_FORMAT_PDF, 0, 1, 0, 200, 1 },
		{ 0, PPS_FILE_FORMAT_PDF, 0, 1, 200, NAN, 1 },
		{ 0, PPS_FILE_FORMAT_PDF, 0, 1, 200, 200, 0 },
		{ 0, PPS_FILE_FORMAT_PDF, 0, 1, 200, 200, 17 },
	};
	for (guint i = 0; i < G_N_ELEMENTS (invalid); i++) {
		g_assert_false (pps_dbus_document_call_export_begin_sync (session->proxy, g_variant_new_handle (invalid[i].handle),
		                                                          invalid[i].format, invalid[i].first, invalid[i].last,
		                                                          invalid[i].width, invalid[i].height, FALSE, invalid[i].nup,
		                                                          G_DBUS_CALL_FLAGS_NONE, 5000, descriptors, NULL, NULL, &error));
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
		g_clear_error (&error);
	}
	g_autoptr (GUnixFDList) readonly = g_unix_fd_list_new ();
	int input = g_open (source_path, O_RDONLY | O_CLOEXEC, 0);
	g_assert_cmpint (input, >=, 0);
	g_assert_cmpint (g_unix_fd_list_append (readonly, input, &error), ==, 0);
	close (input);
	g_assert_no_error (error);
	g_assert_false (pps_dbus_document_call_export_begin_sync (session->proxy, g_variant_new_handle (0), PPS_FILE_FORMAT_PDF,
	                                                          0, 1, 200, 200, FALSE, 1, G_DBUS_CALL_FLAGS_NONE, 5000, readonly, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	int pipes[2];
	g_assert_cmpint (pipe (pipes), ==, 0);
	g_autoptr (GUnixFDList) pipe_descriptors = g_unix_fd_list_new ();
	g_assert_cmpint (g_unix_fd_list_append (pipe_descriptors, pipes[1], &error), ==, 0);
	close (pipes[0]);
	close (pipes[1]);
	g_assert_no_error (error);
	g_assert_false (pps_dbus_document_call_export_begin_sync (session->proxy, g_variant_new_handle (0), PPS_FILE_FORMAT_PDF,
	                                                          0, 1, 200, 200, FALSE, 1, G_DBUS_CALL_FLAGS_NONE, 5000, pipe_descriptors, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	/* The destination has no pathname: only the passed descriptor can reach it. */
	g_assert_true (pps_dbus_document_call_export_begin_sync (session->proxy, g_variant_new_handle (handle), PPS_FILE_FORMAT_PDF,
	                                                         0, 1, 200, 200, FALSE, 1, G_DBUS_CALL_FLAGS_NONE, 5000, descriptors, NULL, NULL, &error));
	g_assert_no_error (error);
	g_assert_false (pps_dbus_document_call_export_page_sync (session->proxy, 2, 0, 1., -1, -1, PPS_RENDER_ANNOTS_ALL,
	                                                         G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	g_assert_true (pps_dbus_document_call_export_begin_page_sync (session->proxy, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error));
	g_assert_no_error (error);
	g_assert_true (pps_dbus_document_call_export_page_sync (session->proxy, 0, 0, 1., -1, -1, PPS_RENDER_ANNOTS_ALL,
	                                                        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error));
	g_assert_no_error (error);
	g_assert_true (pps_dbus_document_call_export_end_page_sync (session->proxy, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error));
	g_assert_no_error (error);
	g_assert_true (pps_dbus_document_call_export_end_sync (session->proxy, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error));
	g_assert_no_error (error);
	g_assert_false (pps_dbus_document_call_export_end_sync (session->proxy, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
	g_clear_error (&error);
	g_assert_cmpint (lseek (fd, 0, SEEK_SET), ==, 0);
	g_autoptr (PpsDocument) exported = _pps_document_factory_create_document ("application/pdf", &error);
	g_assert_no_error (error);
	g_assert_true (pps_document_load_fd (exported, fd, &error));
	g_assert_no_error (error);
	pps_document_setup_cache (exported);
	g_assert_cmpint (pps_document_get_n_pages (exported), ==, 1);
	g_autoptr (PpsPage) page = pps_document_get_page (exported, 0);
	g_autofree char *text = pps_document_text_get_text (PPS_DOCUMENT_TEXT (exported), page);
	g_assert_nonnull (strstr (text, "Page one"));
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
	g_test_add_data_func ("/document-export/proxy-nup-unlocks-document", GINT_TO_POINTER (TRUE), pdf_nup_unlocks_document);
	g_test_add_data_func ("/document-export/native-pdf-vector", GINT_TO_POINTER (FALSE), pdf_vector_export);
	g_test_add_data_func ("/document-export/proxy-pdf-vector", GINT_TO_POINTER (TRUE), pdf_vector_export);
	g_test_add_data_func ("/document-export/native-tiff-postscript", GUINT_TO_POINTER (2), legacy_postscript);
	g_test_add_data_func ("/document-export/proxy-tiff-postscript", GUINT_TO_POINTER (3), legacy_postscript);
	g_test_add_data_func ("/document-export/native-djvu-postscript", GUINT_TO_POINTER (0), legacy_postscript);
	g_test_add_data_func ("/document-export/proxy-djvu-postscript", GUINT_TO_POINTER (1), legacy_postscript);
	g_test_add_func ("/document-export/descriptor-and-errors", export_descriptor_and_errors);
	g_test_add_data_func ("/document-export/native-finalization-failure", GINT_TO_POINTER (FALSE), finalization_failure);
	g_test_add_data_func ("/document-export/proxy-finalization-failure", GINT_TO_POINTER (TRUE), finalization_failure);
	int result = g_test_run ();
	pps_shutdown ();
	return result;
}

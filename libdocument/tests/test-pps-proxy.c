// SPDX-License-Identifier: GPL-2.0-or-later

#include <cairo-pdf.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pps-document-proxy-private.h"
#include "pps-document-security.h"

static char *fixture_dir;
static char *fixture_path;
static char *fixture_uri;

static PpsDocument *
new_document (void)
{
	g_autoptr (GError) error = NULL;
	PpsDocument *document = _pps_document_proxy_new ("application/pdf", NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (document);
	g_assert_true (PPS_IS_DOCUMENT_SECURITY (document));
	return document;
}

static void
load_document (PpsDocument *document, const char *uri)
{
	g_autoptr (GError) error = NULL;
	g_assert_true (pps_document_load (document, uri, &error));
	g_assert_no_error (error);
	pps_document_setup_cache (document);
}

static void
render_page (PpsDocument *document, int index)
{
	g_autoptr (PpsPage) page = pps_document_get_page (document, index);
	g_autoptr (PpsRenderContext) context = pps_render_context_new (page, 0, 1, PPS_RENDER_ANNOTS_ALL);
	cairo_surface_t *surface = pps_document_render (document, context);
	double width, height;

	g_assert_nonnull (surface);
	pps_document_get_page_size (document, index, &width, &height);
	g_assert_cmpint (cairo_image_surface_get_width (surface), ==, (int) width);
	g_assert_cmpint (cairo_image_surface_get_height (surface), ==, (int) height);
	cairo_surface_destroy (surface);
}

static void
metadata_and_rendering (void)
{
	g_autoptr (PpsDocument) document = new_document ();
	PpsDocumentBackendInfo backend;
	double width, height;

	load_document (document, fixture_uri);
	g_assert_cmpint (pps_document_get_n_pages (document), ==, 2);
	PpsDocumentSession *session = _pps_document_proxy_get_session (document);
	pid_t pid = g_ascii_strtoll (g_subprocess_get_identifier (session->process), NULL, 10);
	siginfo_t status;
	g_subprocess_send_signal (session->process, SIGSTOP);
	g_assert_cmpint (waitid (P_PID, pid, &status, WSTOPPED | WNOWAIT), ==, 0);
	/* All layout and document properties remain local, even with a stopped worker. */
	pps_document_get_page_size (document, 0, &width, &height);
	g_assert_cmpfloat (width, ==, 144);
	g_assert_cmpfloat (height, ==, 288);
	pps_document_get_page_size (document, 1, &width, &height);
	g_assert_cmpfloat (width, ==, 288);
	g_assert_cmpfloat (height, ==, 144);
	g_autofree char *first_label = pps_document_get_page_label (document, 0);
	g_autofree char *second_label = pps_document_get_page_label (document, 1);
	g_assert_cmpstr (first_label, ==, "front");
	g_assert_cmpstr (second_label, ==, "A-2");
	g_autoptr (PpsDocumentInfo) info = pps_document_get_info (document);
	g_assert_cmpstr (info->title, ==, "Proxy fixture");
	g_assert_cmpstr (info->author, ==, "Papers tests");
	g_assert_cmpint (info->n_pages, ==, 2);
	g_assert_true (pps_document_get_backend_info (document, &backend));
	g_assert_nonnull (backend.name);
	g_subprocess_send_signal (session->process, SIGCONT);
	render_page (document, 0);
	render_page (document, 1);
	g_assert_cmpuint (g_type_from_name ("PdfDocument"), ==, G_TYPE_INVALID);
}

static void
password_retry (void)
{
	g_autoptr (PpsDocument) document = new_document ();
	g_autoptr (GError) error = NULL;
	g_autofree char *uri = g_filename_to_uri (TESTDATADIR "/PasswordEncrypted.pdf", NULL, NULL);

	g_assert_false (pps_document_load (document, uri, &error));
	g_assert_error (error, PPS_DOCUMENT_ERROR, PPS_DOCUMENT_ERROR_ENCRYPTED);
	g_clear_error (&error);
	pps_document_security_set_password (PPS_DOCUMENT_SECURITY (document), "wrong");
	g_assert_false (pps_document_load (document, uri, &error));
	g_assert_error (error, PPS_DOCUMENT_ERROR, PPS_DOCUMENT_ERROR_ENCRYPTED);
	g_clear_error (&error);
	pps_document_security_set_password (PPS_DOCUMENT_SECURITY (document), "password");
	load_document (document, uri);
	g_assert_cmpint (pps_document_get_n_pages (document), ==, 1);
	render_page (document, 0);
}

static void
fd_and_save (void)
{
	g_autoptr (PpsDocument) document = new_document ();
	g_autoptr (PpsDocument) saved = new_document ();
	g_autoptr (GError) error = NULL;
	g_autofree char *path = g_build_filename (fixture_dir, "saved.pdf", NULL);
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	int original = open (fixture_path, O_RDONLY | O_CLOEXEC);
	/* Keep the consumed descriptor away from normal transport allocations. */
	int fd = fcntl (original, F_DUPFD_CLOEXEC, 128);
	close (original);

	g_assert_cmpint (fd, >=, 0);
	g_assert_true (pps_document_load_fd (document, fd, &error));
	g_assert_no_error (error);
	g_assert_cmpint (fcntl (fd, F_GETFD), ==, -1);
	g_assert_cmpint (errno, ==, EBADF);
	pps_document_setup_cache (document);
	pps_document_set_modified (document, TRUE);
	g_assert_true (pps_document_save (document, uri, &error));
	g_assert_no_error (error);
	g_assert_false (pps_document_get_modified (document));
	load_document (saved, uri);
	g_assert_cmpint (pps_document_get_n_pages (saved), ==, 2);
	render_page (saved, 1);
	g_assert_cmpint (g_unlink (path), ==, 0);
}

static void
worker_failed (PpsDocument *document, GError *error, gboolean *failed)
{
	g_assert_nonnull (error);
	*failed = TRUE;
}

static void
failure_and_lifetime (void)
{
	g_autoptr (PpsDocument) first = new_document ();
	g_autoptr (PpsDocument) second = new_document ();
	g_autoptr (GError) error = NULL;
	gboolean failed = FALSE;

	load_document (first, fixture_uri);
	load_document (second, fixture_uri);
	g_signal_connect (first, "failed", G_CALLBACK (worker_failed), &failed);
	GSubprocess *process = _pps_document_proxy_get_session (first)->process;
	g_subprocess_force_exit (process);
	g_assert_true (g_subprocess_wait (process, NULL, &error));
	g_assert_no_error (error);
	gint64 deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
	while (!failed && g_get_monotonic_time () < deadline)
		g_main_context_iteration (NULL, FALSE);
	g_assert_true (failed);
	g_autoptr (PpsPage) page = pps_document_get_page (first, 0);
	g_autoptr (PpsRenderContext) context = pps_render_context_new (page, 0, 1, PPS_RENDER_ANNOTS_ALL);
	g_assert_null (pps_document_render (first, context));
	render_page (second, 0);
	g_autoptr (GSubprocess) remaining = g_object_ref (_pps_document_proxy_get_session (second)->process);
	g_clear_object (&second);
	g_assert_true (g_subprocess_wait (remaining, NULL, &error));
	g_assert_no_error (error);
}

static void
text_and_find (void)
{
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, fixture_uri);
	g_assert_true (PPS_IS_DOCUMENT_TEXT (document));
	g_assert_true (PPS_IS_DOCUMENT_FIND (document));
	g_autoptr (PpsPage) page = pps_document_get_page (document, 0);
	PpsDocumentText *text_document = PPS_DOCUMENT_TEXT (document);
	PpsDocumentFind *find_document = PPS_DOCUMENT_FIND (document);
	g_autofree char *text = pps_document_text_get_text (text_document, page);
	g_assert_cmpstr (text, ==, "First page\ncontinued\nhyphen-\nation");
	g_autofree PpsRectangle *areas = NULL;
	guint count = 0;
	g_assert_true (pps_document_text_get_text_layout (text_document, page, &areas, &count));
	g_assert_cmpuint (count, ==, g_utf8_strlen (text, -1));
	g_assert_cmpfloat (areas[0].x1, <, areas[0].x2);
	g_assert_cmpfloat (areas[0].y1, <, areas[0].y2);
	cairo_region_t *mapping = pps_document_text_get_text_mapping (text_document, page);
	g_assert_nonnull (mapping);
	g_assert_false (cairo_region_is_empty (mapping));
	cairo_region_destroy (mapping);
	g_autoptr (PangoAttrList) attrs = pps_document_text_get_text_attrs (text_document, page);
	g_assert_nonnull (attrs);
	PangoAttrIterator *iterator = pango_attr_list_get_iterator (attrs);
	PangoAttribute *size = pango_attr_iterator_get (iterator, PANGO_ATTR_SIZE);
	g_assert_nonnull (size);
	g_assert_cmpint (((PangoAttrInt *) size)->value, ==, 16 * PANGO_SCALE);
	pango_attr_iterator_destroy (iterator);
	PpsRectangle area = { areas[0].x1, areas[0].y1, areas[9].x2, areas[0].y2 };
	g_autofree char *part = pps_document_text_get_text_in_area (text_document, page, &area);
	g_assert_cmpstr (part, ==, "First page");
	g_assert_cmpuint (pps_document_find_get_supported_options (find_document), ==,
	                  PPS_FIND_CASE_SENSITIVE | PPS_FIND_WHOLE_WORDS_ONLY);
	GList *matches = pps_document_find_find_text (find_document, page, "first", PPS_FIND_DEFAULT);
	g_assert_cmpuint (g_list_length (matches), ==, 1);
	g_list_free_full (matches, (GDestroyNotify) pps_find_rectangle_free);
	g_assert_null (pps_document_find_find_text (find_document, page, "first", PPS_FIND_CASE_SENSITIVE));
	g_assert_null (pps_document_find_find_text (find_document, page, "Fir", PPS_FIND_WHOLE_WORDS_ONLY));
	matches = pps_document_find_find_text (find_document, page, "page continued", PPS_FIND_DEFAULT);
	g_assert_cmpuint (g_list_length (matches), ==, 2);
	g_assert_true (((PpsFindRectangle *) matches->data)->next_line);
	g_assert_false (((PpsFindRectangle *) matches->data)->after_hyphen);
	g_list_free_full (matches, (GDestroyNotify) pps_find_rectangle_free);
	matches = pps_document_find_find_text (find_document, page, "hyphenation", PPS_FIND_DEFAULT);
	g_assert_cmpuint (g_list_length (matches), ==, 2);
	g_assert_true (((PpsFindRectangle *) matches->data)->next_line);
	g_assert_true (((PpsFindRectangle *) matches->data)->after_hyphen);
	g_list_free_full (matches, (GDestroyNotify) pps_find_rectangle_free);
	area.x1 = NAN;
	g_assert_null (pps_document_text_get_text_in_area (text_document, page, &area));
	render_page (document, 1);
}

static void
create_fixture (void)
{
	g_autoptr (GError) error = NULL;
	fixture_dir = g_dir_make_tmp ("papers-proxy-test-XXXXXX", &error);
	g_assert_no_error (error);
	fixture_path = g_build_filename (fixture_dir, "two-pages.pdf", NULL);
	fixture_uri = g_filename_to_uri (fixture_path, NULL, NULL);
	cairo_surface_t *surface = cairo_pdf_surface_create (fixture_path, 144, 288);
	cairo_pdf_surface_set_metadata (surface, CAIRO_PDF_METADATA_TITLE, "Proxy fixture");
	cairo_pdf_surface_set_metadata (surface, CAIRO_PDF_METADATA_AUTHOR, "Papers tests");
	cairo_t *cr = cairo_create (surface);
	for (int i = 0; i < 2; i++) {
		if (i)
			cairo_pdf_surface_set_size (surface, 288, 144);
		cairo_pdf_surface_set_page_label (surface, i ? "A-2" : "front");
		cairo_set_font_size (cr, 16);
		cairo_move_to (cr, 10, 30);
		cairo_show_text (cr, i ? "Second page" : "First page");
		if (!i) {
			cairo_move_to (cr, 10, 55);
			cairo_show_text (cr, "continued");
			cairo_move_to (cr, 10, 80);
			cairo_show_text (cr, "hyphen-");
			cairo_move_to (cr, 10, 105);
			cairo_show_text (cr, "ation");
		}
		cairo_show_page (cr);
	}
	g_assert_cmpint (cairo_status (cr), ==, CAIRO_STATUS_SUCCESS);
	cairo_destroy (cr);
	cairo_surface_finish (surface);
	g_assert_cmpint (cairo_surface_status (surface), ==, CAIRO_STATUS_SUCCESS);
	cairo_surface_destroy (surface);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	create_fixture ();
	g_test_add_func ("/document-proxy/metadata-and-rendering", metadata_and_rendering);
	g_test_add_func ("/document-proxy/password-retry", password_retry);
	g_test_add_func ("/document-proxy/fd-and-save", fd_and_save);
	g_test_add_func ("/document-proxy/failure-and-lifetime", failure_and_lifetime);
	g_test_add_func ("/document-proxy/text-and-find", text_and_find);
	int result = g_test_run ();
	g_unlink (fixture_path);
	g_rmdir (fixture_dir);
	g_free (fixture_path);
	g_free (fixture_dir);
	g_free (fixture_uri);
	return result;
}

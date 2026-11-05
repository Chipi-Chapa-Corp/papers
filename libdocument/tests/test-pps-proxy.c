// SPDX-License-Identifier: GPL-2.0-or-later

#include <cairo-pdf.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pps-attachment-private.h"
#include "pps-document-annotation-private.h"
#include "pps-document-proxy-private.h"
#include "pps-document-security.h"
#include "pps-form-field-private.h"
#include "pps-init.h"
#include "pps-media-private.h"
#include "pps-outlines.h"

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
	g_assert_true ((pps_file_exporter_get_capabilities (PPS_FILE_EXPORTER (document)) & PPS_FILE_EXPORTER_CAN_GENERATE_PS) != 0);
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
	g_assert_true (pps_document_links_has_document_links (PPS_DOCUMENT_LINKS (document)));
	g_assert_false (pps_document_layers_has_layers (PPS_DOCUMENT_LAYERS (document)));
	g_assert_cmpuint (pps_document_find_get_supported_options (PPS_DOCUMENT_FIND (document)), ==,
	                  PPS_FIND_CASE_SENSITIVE | PPS_FIND_WHOLE_WORDS_ONLY);
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
selection (void)
{
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, fixture_uri);
	/* Match PpsJobRenderTexture: paint the page before its selection. */
	render_page (document, 0);
	g_assert_true (PPS_IS_SELECTION (document));
	PpsSelection *selection = PPS_SELECTION (document);
	g_autoptr (PpsPage) page = pps_document_get_page (document, 0);
	g_autoptr (PpsRenderContext) context = pps_render_context_new (page, 0, 1, PPS_RENDER_ANNOTS_ALL);
	g_autofree PpsRectangle *areas = NULL;
	guint count;
	g_assert_true (pps_document_text_get_text_layout (PPS_DOCUMENT_TEXT (document), page, &areas, &count));
	PpsRectangle points = {
		(areas[0].x1 + areas[0].x2) / 2, (areas[0].y1 + areas[0].y2) / 2,
		(areas[1].x1 + areas[1].x2) / 2, (areas[1].y1 + areas[1].y2) / 2
	};
	const char *expected[] = { "Fi", "First", "First page" };
	for (int style = PPS_SELECTION_STYLE_GLYPH; style <= PPS_SELECTION_STYLE_LINE; style++) {
		g_autofree char *text = pps_selection_get_selected_text (selection, page, style, &points);
		g_assert_cmpstr (text, ==, expected[style]);
		cairo_region_t *region = pps_selection_get_selection_region (selection, context, style, &points);
		g_assert_nonnull (region);
		g_assert_false (cairo_region_is_empty (region));
		cairo_region_destroy (region);
	}
	GdkRGBA text_color = { 0, 0, 0, 1 };
	GdkRGBA base_color = { .2, .4, 1, .5 };
	cairo_surface_t *surface = NULL;
	pps_selection_render_selection (selection, context, &surface, &points, NULL,
	                                PPS_SELECTION_STYLE_GLYPH, &text_color, &base_color);
	g_assert_nonnull (surface);
	g_assert_cmpint (cairo_image_surface_get_width (surface), ==, 144);
	g_assert_cmpint (cairo_image_surface_get_height (surface), ==, 288);
	cairo_surface_t *original = surface;
	pps_selection_render_selection (selection, context, &surface, &points, &points,
	                                PPS_SELECTION_STYLE_WORD, &text_color, &base_color);
	g_assert_true (surface == original);
	cairo_surface_t *fresh = NULL;
	pps_selection_render_selection (selection, context, &fresh, &points, NULL,
	                                PPS_SELECTION_STYLE_WORD, &text_color, &base_color);
	g_assert_nonnull (fresh);
	cairo_surface_flush (surface);
	cairo_surface_flush (fresh);
	gsize size = cairo_image_surface_get_stride (surface) * 288;
	g_assert_cmpmem (cairo_image_surface_get_data (surface), size,
	                 cairo_image_surface_get_data (fresh), size);
	cairo_surface_destroy (surface);
	cairo_surface_destroy (fresh);
	context->rotation = 90;
	context->scale = 0;
	pps_render_context_set_target_size (context, 160, 200);
	surface = NULL;
	pps_selection_render_selection (selection, context, &surface, &points, NULL,
	                                PPS_SELECTION_STYLE_LINE, &text_color, &base_color);
	g_assert_nonnull (surface);
	/* Selection surfaces use unrotated page coordinates, as the native backend does. */
	g_assert_cmpint (cairo_image_surface_get_width (surface), ==, 200);
	g_assert_cmpint (cairo_image_surface_get_height (surface), ==, 160);
	cairo_surface_destroy (surface);
	g_assert_null (pps_selection_get_selected_text (selection, page, 99, &points));
	points.x1 = NAN;
	g_assert_null (pps_selection_get_selection_region (selection, context, PPS_SELECTION_STYLE_LINE, &points));
	render_page (document, 1);
}

static void
links_and_outlines (void)
{
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, fixture_uri);
	g_assert_true (PPS_IS_DOCUMENT_LINKS (document));
	PpsDocumentLinks *links = PPS_DOCUMENT_LINKS (document);
	g_assert_true (pps_document_links_has_document_links (links));
	g_autoptr (GListModel) model = pps_document_links_get_links_model (links);
	g_assert_nonnull (model);
	g_assert_cmpuint (g_list_model_get_n_items (model), ==, 1);
	g_autoptr (PpsOutlines) root = g_list_model_get_item (model, 0);
	g_assert_true (pps_outlines_get_expand (root));
	g_autofree char *markup = NULL, *label = NULL;
	g_object_get (root, "markup", &markup, "label", &label, NULL);
	g_assert_cmpstr (markup, ==, "Chapter &amp; appendix");
	g_assert_null (label);
	label = pps_document_links_get_link_page_label (links, pps_outlines_get_link (root));
	g_assert_cmpstr (label, ==, "A-2");
	g_assert_cmpint (pps_document_links_get_link_page (links, pps_outlines_get_link (root)), ==, 1);
	GListModel *children = pps_outlines_get_children (root);
	g_assert_nonnull (children);
	g_assert_cmpuint (g_list_model_get_n_items (children), ==, 1);
	g_autoptr (PpsOutlines) child = g_list_model_get_item (children, 0);
	g_assert_cmpint (pps_document_links_get_link_page (links, pps_outlines_get_link (child)), ==, 0);
	g_autoptr (PpsLinkDest) dest = pps_document_links_find_link_dest (links, "second");
	g_assert_nonnull (dest);
	g_assert_cmpint (pps_document_links_get_dest_page (links, dest), ==, 1);
	g_assert_cmpint (pps_document_links_find_link_page (links, "second"), ==, 1);
	g_assert_null (pps_document_links_find_link_dest (links, "missing"));
	g_assert_cmpint (pps_document_links_find_link_page (links, "missing"), ==, -1);
	g_autoptr (PpsPage) page = pps_document_get_page (document, 0);
	PpsMappingList *mappings = pps_document_links_get_links (links, page);
	g_assert_nonnull (mappings);
	g_assert_cmpuint (pps_mapping_list_get_page (mappings), ==, 0);
	g_assert_cmpuint (pps_mapping_list_length (mappings), ==, 1);
	PpsMapping *mapping = pps_mapping_list_nth (mappings, 0);
	g_assert_cmpfloat (mapping->area.x1, <, mapping->area.x2);
	g_assert_cmpint (pps_document_links_get_link_page (links, mapping->data), ==, 1);
	pps_mapping_list_unref (mappings);
	g_assert_false (pps_document_layers_has_layers (PPS_DOCUMENT_LAYERS (document)));
	g_assert_null (pps_document_layers_get_layers (PPS_DOCUMENT_LAYERS (document)));
}

static char *
write_pdf_fixture (const char *name, const char *const *objects, guint n_objects)
{
	g_autoptr (GString) pdf = g_string_new ("%PDF-1.7\n");
	g_autofree gsize *offsets = g_new (gsize, n_objects);
	for (guint i = 0; i < n_objects; i++) {
		offsets[i] = pdf->len;
		g_string_append_printf (pdf, "%u 0 obj\n%s\nendobj\n", i + 1, objects[i]);
	}
	gsize xref = pdf->len;
	g_string_append_printf (pdf, "xref\n0 %u\n0000000000 65535 f \n", (guint) n_objects + 1);
	for (guint i = 0; i < n_objects; i++)
		g_string_append_printf (pdf, "%010" G_GSIZE_FORMAT " 00000 n \n", offsets[i]);
	g_string_append_printf (pdf, "trailer\n<< /Size %u /Root 1 0 R >>\nstartxref\n%" G_GSIZE_FORMAT "\n%%%%EOF\n",
	                        (guint) n_objects + 1, xref);
	g_autofree char *path = g_build_filename (fixture_dir, name, NULL);
	g_autoptr (GError) error = NULL;
	g_assert_true (g_file_set_contents (path, pdf->str, pdf->len, &error));
	g_assert_no_error (error);
	return g_steal_pointer (&path);
}

static char *
create_layers_fixture (void)
{
	const char *content = "/OC /Red BDC 1 0 0 rg 0 0 100 100 re f EMC\n"
			      "/OC /Blue BDC 0 0 1 rg 0 0 100 100 re f EMC\n";
	g_autofree char *stream = g_strdup_printf ("<< /Length %zu >>\nstream\n%sendstream", strlen (content), content);
	const char *objects[] = {
		"<< /Type /Catalog /Pages 2 0 R /OCProperties << /OCGs [5 0 R 6 0 R] /D << /Order [5 0 R 6 0 R] /ON [5 0 R] /OFF [6 0 R] /RBGroups [[5 0 R 6 0 R]] >> >> >>",
		"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
		"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << /Properties << /Red 5 0 R /Blue 6 0 R >> >> /Contents 4 0 R /Annots [7 0 R] >>",
		stream,
		"<< /Type /OCG /Name (Red) >>",
		"<< /Type /OCG /Name (Blue) >>",
		"<< /Type /Annot /Subtype /Link /Rect [0 0 20 20] /A << /S /SetOCGState /State [/OFF 5 0 R /ON 6 0 R] >> >>"
	};
	return write_pdf_fixture ("layers.pdf", objects, G_N_ELEMENTS (objects));
}

static char *
create_forms_fixture (void)
{
	const char *objects[] = {
		"<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R >>",
		"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
		"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] /Resources << /Font << /Helv 6 0 R >> >> /Contents 4 0 R /Annots [7 0 R 8 0 R 9 0 R 10 0 R 11 0 R 13 0 R 14 0 R 17 0 R 18 0 R 19 0 R] >>",
		"<< /Length 0 >>\nstream\nendstream",
		"<< /Fields [7 0 R 8 0 R 9 0 R 10 0 R 11 0 R 12 0 R 17 0 R 18 0 R 19 0 R] /DR << /Font << /Helv 6 0 R >> >> /DA (/Helv 12 Tf 0 g) /NeedAppearances true >>",
		"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
		"<< /Type /Annot /Subtype /Widget /FT /Tx /T (text) /TU (Text) /V (initial) /DV (default) /DA (/Helv 12 Tf 0 g) /MaxLen 42 /Rect [10 350 150 375] /P 3 0 R >>",
		"<< /Type /Annot /Subtype /Widget /FT /Tx /T (secret) /TU (Secret) /Ff 12288 /V (secret) /Rect [10 300 150 340] /P 3 0 R >>",
		"<< /Type /Annot /Subtype /Widget /FT /Btn /T (check) /TU (Check) /V /Off /DV /Off /AS /Off /AP << /N << /Off 15 0 R /Yes 16 0 R >> >> /Rect [10 260 30 280] /P 3 0 R >>",
		"<< /Type /Annot /Subtype /Widget /FT /Ch /T (list) /TU (List) /Ff 2097152 /Opt [(Red) (Green) (Blue)] /V [(Red)] /DV [(Red)] /I [0] /Rect [50 200 150 280] /P 3 0 R >>",
		"<< /Type /Annot /Subtype /Widget /FT /Ch /T (combo) /TU (Combo) /Ff 393216 /Opt [(One) (Two)] /V (One) /DV (One) /Rect [10 160 150 185] /P 3 0 R >>",
		"<< /FT /Btn /T (radio) /Ff 32768 /V /A /DV /A /Kids [13 0 R 14 0 R] >>",
		"<< /Type /Annot /Subtype /Widget /Parent 12 0 R /TU (Radio A) /AS /A /AP << /N << /Off 15 0 R /A 16 0 R >> >> /Rect [10 120 30 140] /P 3 0 R >>",
		"<< /Type /Annot /Subtype /Widget /Parent 12 0 R /TU (Radio B) /AS /Off /AP << /N << /Off 15 0 R /B 16 0 R >> >> /Rect [40 120 60 140] /P 3 0 R >>",
		"<< /Type /XObject /Subtype /Form /BBox [0 0 20 20] /Length 0 >>\nstream\nendstream",
		"<< /Type /XObject /Subtype /Form /BBox [0 0 20 20] /Length 24 >>\nstream\n0 0 0 rg 0 0 20 20 re f\nendstream",
		"<< /Type /Annot /Subtype /Widget /FT /Btn /Ff 65536 /T (reset) /TU (Reset) /A << /S /ResetForm /Fields [(text)] /Flags 0 >> /Rect [10 80 100 105] /P 3 0 R >>",
		"<< /Type /Annot /Subtype /Widget /FT /Sig /T (signature) /TU (Signature) /Rect [10 40 150 65] /P 3 0 R >>",
		"<< /Type /Annot /Subtype /Widget /FT /Tx /T (readonly) /TU (Read only) /Ff 1 /V (fixed) /Rect [180 350 320 375] /P 3 0 R >>"
	};
	return write_pdf_fixture ("forms.pdf", objects, G_N_ELEMENTS (objects));
}

static PpsFormField *
find_field (PpsMappingList *mappings, const char *name)
{
	for (guint i = 0; i < pps_mapping_list_length (mappings); i++) {
		PpsFormField *field = pps_mapping_list_nth (mappings, i)->data;
		if (g_strcmp0 (pps_form_field_get_alternate_name (field), name) == 0)
			return field;
	}
	g_error ("Missing form field %s", name);
}

static void
assert_form_text (PpsDocumentForms *forms, PpsFormField *field, const char *expected)
{
	g_autofree char *text = PPS_IS_FORM_FIELD_TEXT (field)
	                            ? pps_document_forms_form_field_text_get_text (forms, field)
	                            : pps_document_forms_form_field_choice_get_text (forms, field);
	g_assert_cmpstr (text, ==, expected);
}

static void
modified_notified (PpsDocument *document, GParamSpec *property, guint *count)
{
	(*count)++;
}

static void
forms_edit_and_save (void)
{
	g_autofree char *path = create_forms_fixture ();
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, uri);
	g_assert_true (PPS_IS_DOCUMENT_FORMS (document));
	PpsDocumentForms *forms = PPS_DOCUMENT_FORMS (document);
	g_autoptr (PpsPage) page = pps_document_get_page (document, 0);
	PpsMappingList *mappings = pps_document_forms_get_form_fields (forms, page);
	g_assert_nonnull (mappings);
	g_assert_cmpuint (pps_mapping_list_get_page (mappings), ==, 0);
	g_assert_cmpuint (pps_mapping_list_length (mappings), ==, 10);
	PpsFormField *text = find_field (mappings, "Text");
	PpsFormField *secret = find_field (mappings, "Secret");
	PpsFormField *check = find_field (mappings, "Check");
	PpsFormField *list = find_field (mappings, "List");
	PpsFormField *combo = find_field (mappings, "Combo");
	PpsFormField *radio_a = pps_mapping_list_get_data (mappings, &(PpsPoint){ 20, 270 });
	PpsFormField *radio_b = pps_mapping_list_get_data (mappings, &(PpsPoint){ 50, 270 });
	PpsFormField *reset = find_field (mappings, "Reset");
	g_assert_true (PPS_IS_FORM_FIELD_SIGNATURE (find_field (mappings, "Signature")));
	g_assert_true (find_field (mappings, "Read only")->is_read_only);
	g_assert_cmpint (text->page->index, ==, 0);
	g_assert_null (text->page->backend_page);
	/* Poppler reports the automatic/default size as zero for this fixture. */
	g_assert_cmpfloat (text->font_size, ==, 0);
	g_assert_cmpint (PPS_FORM_FIELD_TEXT (text)->max_len, ==, 42);
	g_assert_cmpint (PPS_FORM_FIELD_TEXT (secret)->type, ==, PPS_FORM_FIELD_TEXT_MULTILINE);
	g_assert_true (PPS_FORM_FIELD_TEXT (secret)->is_password);
	g_assert_true (PPS_FORM_FIELD_CHOICE (list)->multi_select);
	g_assert_true (PPS_FORM_FIELD_CHOICE (combo)->is_editable);
	g_assert_cmpint (PPS_FORM_FIELD_BUTTON (radio_a)->type, ==, PPS_FORM_FIELD_BUTTON_RADIO);
	g_assert_cmpint (PPS_FORM_FIELD_BUTTON (reset)->type, ==, PPS_FORM_FIELD_BUTTON_PUSH);
	g_assert_cmpint (pps_link_action_get_action_type (pps_link_get_action (reset->activation_link)), ==, PPS_LINK_ACTION_TYPE_RESET_FORM);
	assert_form_text (forms, text, "initial");
	g_assert_false (pps_document_get_modified (document));
	g_assert_false (pps_document_forms_document_is_modified (forms));
	guint notifications = 0;
	g_signal_connect (document, "notify::modified", G_CALLBACK (modified_notified), &notifications);
	/* Separate page caches must not share their overlays' temporary edit state. */
	g_free (PPS_FORM_FIELD_TEXT (text)->text);
	PPS_FORM_FIELD_TEXT (text)->text = g_strdup ("Pending overlay edit");
	text->changed = TRUE;
	PpsMappingList *again = pps_document_forms_get_form_fields (forms, page);
	PpsFormField *other_text = find_field (again, "Text");
	g_assert_true (other_text != text);
	g_assert_false (other_text->changed);
	g_assert_cmpstr (PPS_FORM_FIELD_TEXT (other_text)->text, ==, "initial");
	g_assert_cmpuint (_pps_document_objects_get_id (_pps_document_proxy_get_objects (document), G_OBJECT (other_text)), ==,
	                  _pps_document_objects_get_id (_pps_document_proxy_get_objects (document), G_OBJECT (text)));
	g_assert_true (text->changed);
	g_assert_cmpstr (PPS_FORM_FIELD_TEXT (text)->text, ==, "Pending overlay edit");
	pps_mapping_list_unref (again);
	pps_document_forms_form_field_text_set_text (forms, text, "Edited α");
	assert_form_text (forms, text, "Edited α");
	g_assert_true (pps_document_get_modified (document));
	g_assert_true (pps_document_forms_document_is_modified (forms));
	g_assert_cmpuint (notifications, ==, 1);
	pps_document_forms_form_field_choice_set_text (forms, combo, "Custom β");
	assert_form_text (forms, combo, "Custom β");
	g_assert_false (pps_document_forms_form_field_button_get_state (forms, check));
	pps_document_forms_form_field_button_set_state (forms, check, TRUE);
	g_assert_true (pps_document_forms_form_field_button_get_state (forms, check));
	g_assert_cmpint (pps_document_forms_form_field_choice_get_n_items (forms, list), ==, 3);
	g_autofree char *item = pps_document_forms_form_field_choice_get_item (forms, list, 1);
	g_assert_cmpstr (item, ==, "Green");
	g_assert_true (pps_document_forms_form_field_choice_is_item_selected (forms, list, 0));
	pps_document_forms_form_field_choice_unselect_all (forms, list);
	pps_document_forms_form_field_choice_select_item (forms, list, 1);
	pps_document_forms_form_field_choice_select_item (forms, list, 2);
	g_assert_true (pps_document_forms_form_field_choice_is_item_selected (forms, list, 1));
	g_assert_true (pps_document_forms_form_field_choice_is_item_selected (forms, list, 2));
	pps_document_forms_form_field_choice_toggle_item (forms, list, 2);
	g_assert_false (pps_document_forms_form_field_choice_is_item_selected (forms, list, 0));
	g_assert_false (pps_document_forms_form_field_choice_is_item_selected (forms, list, 2));
	g_assert_true (pps_document_forms_form_field_button_get_state (forms, radio_a));
	pps_document_forms_form_field_button_set_state (forms, radio_b, TRUE);
	g_assert_false (pps_document_forms_form_field_button_get_state (forms, radio_a));
	g_assert_true (pps_document_forms_form_field_button_get_state (forms, radio_b));
	g_autoptr (GError) error = NULL;
	g_autofree char *saved_path = g_build_filename (fixture_dir, "forms-saved.pdf", NULL);
	g_autofree char *saved_uri = g_filename_to_uri (saved_path, NULL, NULL);
	g_assert_true (pps_document_save (document, saved_uri, &error));
	g_assert_no_error (error);
	g_assert_false (pps_document_get_modified (document));
	g_assert_false (pps_document_forms_document_is_modified (forms));
	g_assert_cmpuint (notifications, ==, 2);
	g_autoptr (PpsDocument) saved = new_document ();
	load_document (saved, saved_uri);
	g_autoptr (PpsPage) saved_page = pps_document_get_page (saved, 0);
	PpsMappingList *saved_fields = pps_document_forms_get_form_fields (PPS_DOCUMENT_FORMS (saved), saved_page);
	assert_form_text (PPS_DOCUMENT_FORMS (saved), find_field (saved_fields, "Text"), "Edited α");
	assert_form_text (PPS_DOCUMENT_FORMS (saved), find_field (saved_fields, "Combo"), "Custom β");
	g_assert_true (pps_document_forms_form_field_button_get_state (PPS_DOCUMENT_FORMS (saved), find_field (saved_fields, "Check")));
	g_assert_true (pps_document_forms_form_field_choice_is_item_selected (PPS_DOCUMENT_FORMS (saved), find_field (saved_fields, "List"), 1));
	g_assert_true (pps_document_forms_form_field_button_get_state (PPS_DOCUMENT_FORMS (saved), pps_mapping_list_get_data (saved_fields, &(PpsPoint){ 50, 270 })));
	pps_mapping_list_unref (saved_fields);
	/* The reset button must retain its field list across the process boundary. */
	pps_document_forms_reset_form (forms, pps_link_get_action (reset->activation_link));
	assert_form_text (forms, text, "default");
	assert_form_text (forms, combo, "Custom β");
	g_assert_true (pps_document_forms_form_field_button_get_state (forms, check));
	g_assert_true (pps_document_get_modified (document));
	g_assert_true (pps_document_forms_document_is_modified (forms));
	g_assert_cmpuint (notifications, ==, 3);
	pps_document_forms_form_field_text_set_text (forms, text, "Keep this");
	g_autoptr (PpsLinkAction) exclude = pps_link_action_new_reset_form (g_list_append (NULL, g_strdup ("text")), TRUE);
	pps_document_forms_reset_form (forms, exclude);
	assert_form_text (forms, text, "Keep this");
	/* Native reset restores the selection and clears the editable text. */
	assert_form_text (forms, combo, NULL);
	g_assert_true (pps_document_forms_form_field_choice_is_item_selected (forms, combo, 0));
	g_assert_false (pps_document_forms_form_field_button_get_state (forms, check));
	g_autoptr (PpsLinkAction) all = pps_link_action_new_reset_form (NULL, FALSE);
	pps_document_forms_reset_form (forms, all);
	assert_form_text (forms, text, "default");
	again = pps_document_forms_get_form_fields (forms, page);
	g_assert_cmpstr (PPS_FORM_FIELD_TEXT (find_field (again, "Text"))->text, ==, "default");
	g_assert_false (find_field (again, "Text")->changed);
	g_assert_false (PPS_FORM_FIELD_BUTTON (find_field (again, "Check"))->state);
	pps_mapping_list_unref (again);
	pps_mapping_list_unref (mappings);
	g_assert_cmpint (g_unlink (saved_path), ==, 0);
	g_assert_cmpint (g_unlink (path), ==, 0);
}

static void
forms_isolation_and_errors (void)
{
	g_autofree char *path = create_forms_fixture ();
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	g_autoptr (PpsDocument) first = new_document ();
	g_autoptr (PpsDocument) second = new_document ();
	g_autoptr (PpsDocument) plain = new_document ();
	load_document (first, uri);
	load_document (second, uri);
	load_document (plain, fixture_uri);
	PpsDocumentForms *forms = PPS_DOCUMENT_FORMS (first);
	g_autoptr (PpsPage) page = pps_document_get_page (first, 0);
	g_assert_null (pps_document_forms_get_form_fields (PPS_DOCUMENT_FORMS (plain), page));
	PpsMappingList *fields = pps_document_forms_get_form_fields (forms, page);
	PpsMappingList *other_fields = pps_document_forms_get_form_fields (PPS_DOCUMENT_FORMS (second), page);
	PpsFormField *text = find_field (fields, "Text");
	pps_document_forms_form_field_text_set_text (forms, text, "First document");
	pps_document_forms_form_field_text_set_text (PPS_DOCUMENT_FORMS (second), text, "Foreign field");
	assert_form_text (forms, text, "First document");
	assert_form_text (PPS_DOCUMENT_FORMS (second), find_field (other_fields, "Text"), "initial");
	g_assert_false (pps_document_get_modified (second));
	g_assert_false (pps_document_forms_document_is_modified (PPS_DOCUMENT_FORMS (second)));
	PpsDocumentSession *session = _pps_document_proxy_get_session (first);
	PpsDocumentObjects *objects = _pps_document_proxy_get_objects (first);
	guint64 text_id = _pps_document_objects_get_id (objects, G_OBJECT (text));
	guint64 list_id = _pps_document_objects_get_id (objects, G_OBJECT (find_field (fields, "List")));
	g_autoptr (GError) error = NULL;
	g_autofree char *value = NULL;
	gboolean present;
	guint state;
	g_assert_false (pps_dbus_document_call_get_form_text_sync (session->proxy, G_MAXUINT64,
	                                                           G_DBUS_CALL_FLAGS_NONE, 5000, &present, &value, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	g_assert_false (pps_dbus_document_call_set_form_button_state_sync (session->proxy, text_id, TRUE,
	                                                                   G_DBUS_CALL_FLAGS_NONE, 5000, &state, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	g_assert_false (pps_dbus_document_call_get_form_text_sync (session->proxy, list_id,
	                                                           G_DBUS_CALL_FLAGS_NONE, 5000, &present, &value, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	for (int index = -1; index <= 3; index += 4) {
		g_assert_false (pps_dbus_document_call_get_form_choice_item_sync (session->proxy, list_id, index,
		                                                                  G_DBUS_CALL_FLAGS_NONE, 5000, &present, &value, NULL, &error));
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
		g_clear_error (&error);
		g_assert_false (pps_dbus_document_call_select_form_choice_sync (session->proxy, list_id, index, FALSE,
		                                                                G_DBUS_CALL_FLAGS_NONE, 5000, &state, NULL, &error));
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
		g_clear_error (&error);
	}
	g_autoptr (GVariant) mappings = NULL;
	g_assert_false (pps_dbus_document_call_get_form_fields_sync (session->proxy, 100,
	                                                             G_DBUS_CALL_FLAGS_NONE, 5000, &present, &mappings, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	/* Close-prompt state must remain local even while a worker cannot respond. */
	pid_t pid = g_ascii_strtoll (g_subprocess_get_identifier (session->process), NULL, 10);
	g_subprocess_send_signal (session->process, SIGSTOP);
	siginfo_t status;
	g_assert_cmpint (waitid (P_PID, pid, &status, WSTOPPED | WNOWAIT), ==, 0);
	gboolean modified = pps_document_get_modified (first);
	gboolean forms_modified = pps_document_forms_document_is_modified (forms);
	g_subprocess_send_signal (session->process, SIGCONT);
	g_assert_true (modified);
	g_assert_true (forms_modified);
	g_autofree char *missing_path = g_build_filename (fixture_dir, "missing", "saved.pdf", NULL);
	g_autofree char *missing_uri = g_filename_to_uri (missing_path, NULL, NULL);
	g_assert_false (pps_document_save (first, missing_uri, &error));
	g_assert_nonnull (error);
	g_clear_error (&error);
	g_assert_true (pps_document_get_modified (first));
	g_assert_true (pps_document_forms_document_is_modified (forms));
	assert_form_text (forms, text, "First document");
	render_page (first, 0);
	pps_mapping_list_unref (fields);
	pps_mapping_list_unref (other_fields);
	g_assert_cmpint (g_unlink (path), ==, 0);
}

static guint32
pixel_color (PpsDocument *document, int x, int y)
{
	g_autoptr (PpsPage) page = pps_document_get_page (document, 0);
	g_autoptr (PpsRenderContext) context = pps_render_context_new (page, 0, 1, PPS_RENDER_ANNOTS_ALL);
	cairo_surface_t *surface = pps_document_render (document, context);
	g_assert_nonnull (surface);
	cairo_surface_flush (surface);
	guint32 pixel;
	memcpy (&pixel, cairo_image_surface_get_data (surface) + y * cairo_image_surface_get_stride (surface) + x * 4, 4);
	cairo_surface_destroy (surface);
	return pixel & 0xffffff;
}

static guint32
page_color (PpsDocument *document)
{
	return pixel_color (document, 50, 50);
}

static void
embedded_attachments (void)
{
	const char *objects[] = {
		"<< /Type /Catalog /Pages 2 0 R /Names << /EmbeddedFiles << /Names [(data.bin) 5 0 R (empty.txt) 7 0 R] >> >> >>",
		"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
		"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> /Contents 4 0 R >>",
		"<< /Length 0 >>\nstream\nendstream",
		"<< /Type /Filespec /F (data.bin) /Desc (Binary payload) /EF << /F 6 0 R >> >>",
		"<< /Type /EmbeddedFile /Filter /ASCIIHexDecode /Params << /Size 5 /CreationDate (D:20200102030405+05'30') /ModDate (D:20240324112233Z) >> /Length 11 >>\nstream\n00ff610062>\nendstream",
		"<< /Type /Filespec /F (empty.txt) /EF << /F 8 0 R >> >>",
		"<< /Type /EmbeddedFile /Length 0 >>\nstream\nendstream"
	};
	g_autofree char *path = write_pdf_fixture ("attachments.pdf", objects, G_N_ELEMENTS (objects));
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, uri);
	g_assert_true (PPS_IS_DOCUMENT_ATTACHMENTS (document));
	PpsDocumentAttachments *attachments = PPS_DOCUMENT_ATTACHMENTS (document);
	PpsDocumentSession *session = _pps_document_proxy_get_session (document);
	pid_t pid = g_ascii_strtoll (g_subprocess_get_identifier (session->process), NULL, 10);
	g_subprocess_send_signal (session->process, SIGSTOP);
	siginfo_t status;
	g_assert_cmpint (waitid (P_PID, pid, &status, WSTOPPED | WNOWAIT), ==, 0);
	gboolean present = pps_document_attachments_has_attachments (attachments);
	g_subprocess_send_signal (session->process, SIGCONT);
	g_assert_true (present);
	g_autolist (PpsAttachment) items = pps_document_attachments_get_attachments (attachments);
	g_assert_cmpuint (g_list_length (items), ==, 2);
	PpsAttachment *binary = items->data;
	PpsAttachment *empty = items->next->data;
	g_assert_cmpstr (pps_attachment_get_name (binary), ==, "data.bin");
	g_assert_cmpstr (pps_attachment_get_description (binary), ==, "Binary payload");
	g_assert_cmpstr (pps_attachment_get_name (empty), ==, "empty.txt");
	g_autoptr (GDateTime) created = g_date_time_new_from_iso8601 ("2020-01-02T03:04:05+05:30", NULL);
	g_autoptr (GDateTime) modified = g_date_time_new_from_iso8601 ("2024-03-24T11:22:33Z", NULL);
	g_assert_true (g_date_time_equal (created, pps_attachment_get_creation_datetime (binary)));
	g_assert_true (g_date_time_equal (modified, pps_attachment_get_modification_datetime (binary)));
	/* Payloads must be usable after the source document and worker are gone. */
	g_clear_object (&document);
	g_autofree char *binary_path = g_build_filename (fixture_dir, "extracted.bin", NULL);
	g_autofree char *empty_path = g_build_filename (fixture_dir, "extracted-empty.txt", NULL);
	g_autoptr (GFile) binary_file = g_file_new_for_path (binary_path);
	g_autoptr (GFile) empty_file = g_file_new_for_path (empty_path);
	g_autoptr (GError) error = NULL;
	g_assert_true (pps_attachment_save (binary, binary_file, &error));
	g_assert_no_error (error);
	g_assert_true (pps_attachment_save (empty, empty_file, &error));
	g_assert_no_error (error);
	g_autofree char *data = NULL;
	gsize size;
	g_assert_true (g_file_get_contents (binary_path, &data, &size, &error));
	g_assert_no_error (error);
	g_assert_cmpmem (data, size, "\0\xff"
	                             "a\0b",
	                 5);
	g_clear_pointer (&data, g_free);
	g_assert_true (g_file_get_contents (empty_path, &data, &size, &error));
	g_assert_no_error (error);
	g_assert_cmpuint (size, ==, 0);
	g_autoptr (PpsDocument) plain = new_document ();
	load_document (plain, fixture_uri);
	g_assert_false (pps_document_attachments_has_attachments (PPS_DOCUMENT_ATTACHMENTS (plain)));
	g_assert_null (pps_document_attachments_get_attachments (PPS_DOCUMENT_ATTACHMENTS (plain)));
	g_assert_cmpint (g_unlink (binary_path), ==, 0);
	g_assert_cmpint (g_unlink (empty_path), ==, 0);
	g_assert_cmpint (g_unlink (path), ==, 0);
}

static void
attachment_batches (void)
{
	const guint count = 257;
	g_autoptr (GString) names = g_string_new (NULL);
	for (guint i = 0; i < count; i++)
		g_string_append_printf (names, "(file%03u.txt) %u 0 R ", i, 5 + 2 * i);
	g_autoptr (GPtrArray) objects = g_ptr_array_new_with_free_func (g_free);
	g_ptr_array_add (objects, g_strdup_printf ("<< /Type /Catalog /Pages 2 0 R /Names << /EmbeddedFiles << /Names [%s] >> >> >>", names->str));
	g_ptr_array_add (objects, g_strdup ("<< /Type /Pages /Count 1 /Kids [3 0 R] >>"));
	g_ptr_array_add (objects, g_strdup ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> /Contents 4 0 R >>"));
	g_ptr_array_add (objects, g_strdup ("<< /Length 0 >>\nstream\nendstream"));
	for (guint i = 0; i < count; i++) {
		g_ptr_array_add (objects, g_strdup_printf ("<< /Type /Filespec /F (file%03u.txt) /EF << /F %u 0 R >> >>", i, 6 + 2 * i));
		g_ptr_array_add (objects, g_strdup ("<< /Type /EmbeddedFile /Length 0 >>\nstream\nendstream"));
	}
	g_autofree char *path = write_pdf_fixture ("many-attachments.pdf", (const char *const *) objects->pdata, objects->len);
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, uri);
	g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (_pps_document_proxy_get_session (document)->proxy), 5000);
	g_autolist (PpsAttachment) attachments = pps_document_attachments_get_attachments (PPS_DOCUMENT_ATTACHMENTS (document));
	g_assert_cmpuint (g_list_length (attachments), ==, count);
	g_assert_cmpstr (pps_attachment_get_name (attachments->data), ==, "file000.txt");
	g_assert_cmpstr (pps_attachment_get_name (g_list_last (attachments)->data), ==, "file256.txt");
	g_assert_cmpint (g_unlink (path), ==, 0);
}

static void
layers_and_layer_actions (void)
{
	g_autofree char *path = create_layers_fixture ();
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, uri);
	PpsDocumentLayers *layers = PPS_DOCUMENT_LAYERS (document);
	g_assert_true (pps_document_layers_has_layers (layers));
	g_autoptr (GListModel) model = pps_document_layers_get_layers (layers);
	g_assert_nonnull (model);
	g_assert_cmpuint (g_list_model_get_n_items (model), ==, 2);
	g_autoptr (PpsLayer) red = g_list_model_get_item (model, 0);
	g_autoptr (PpsLayer) blue = g_list_model_get_item (model, 1);
	g_assert_true (pps_document_layers_layer_is_visible (layers, red));
	g_assert_false (pps_document_layers_layer_is_visible (layers, blue));
	g_assert_cmpint (pps_layer_get_rb_group (red), ==, pps_layer_get_rb_group (blue));
	g_assert_cmphex (page_color (document), ==, 0xff0000);
	pps_document_layers_hide_layer (layers, red);
	g_assert_cmphex (page_color (document), ==, 0xffffff);
	pps_document_layers_show_layer (layers, red);
	g_autoptr (PpsPage) page = pps_document_get_page (document, 0);
	PpsMappingList *mappings = pps_document_links_get_links (PPS_DOCUMENT_LINKS (document), page);
	g_assert_nonnull (mappings);
	PpsLinkAction *action = pps_link_get_action (pps_mapping_list_nth (mappings, 0)->data);
	g_assert_cmpint (pps_link_action_get_action_type (action), ==, PPS_LINK_ACTION_TYPE_LAYERS_STATE);
	pps_document_layers_show_layer (layers, pps_link_action_get_show_list (action)->data);
	pps_document_layers_hide_layer (layers, pps_link_action_get_hide_list (action)->data);
	g_assert_true (pps_document_layers_layer_is_visible (layers, blue));
	g_assert_false (pps_document_layers_layer_is_visible (layers, red));
	g_assert_cmphex (page_color (document), ==, 0x0000ff);
	PpsMappingList *again = pps_document_links_get_links (PPS_DOCUMENT_LINKS (document), page);
	PpsLinkAction *same_action = pps_link_get_action (pps_mapping_list_nth (again, 0)->data);
	g_assert_true (pps_link_action_get_show_list (action)->data == pps_link_action_get_show_list (same_action)->data);
	pps_mapping_list_unref (again);
	pps_mapping_list_unref (mappings);
	g_autoptr (GError) error = NULL;
	gboolean visible;
	g_assert_false (pps_dbus_document_call_get_layer_visibility_sync (
	    _pps_document_proxy_get_session (document)->proxy, G_MAXUINT64,
	    G_DBUS_CALL_FLAGS_NONE, 5000, &visible, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_autoptr (PpsDocument) other = new_document ();
	load_document (other, uri);
	pps_document_layers_hide_layer (PPS_DOCUMENT_LAYERS (other), blue);
	g_assert_true (pps_document_layers_layer_is_visible (layers, blue));
	g_assert_cmphex (page_color (other), ==, 0xff0000);
	g_assert_cmpint (g_unlink (path), ==, 0);
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
	int outline = cairo_pdf_surface_add_outline (surface, CAIRO_PDF_OUTLINE_ROOT,
	                                             "Chapter & appendix", "dest='second'", CAIRO_PDF_OUTLINE_FLAG_OPEN);
	cairo_pdf_surface_add_outline (surface, outline, "Nested", "page=1 pos=[10 20]", 0);
	cairo_t *cr = cairo_create (surface);
	for (int i = 0; i < 2; i++) {
		if (i)
			cairo_pdf_surface_set_size (surface, 288, 144);
		cairo_pdf_surface_set_page_label (surface, i ? "A-2" : "front");
		cairo_set_font_size (cr, 16);
		cairo_move_to (cr, 10, 30);
		if (!i)
			cairo_tag_begin (cr, CAIRO_TAG_LINK, "dest='second'");
		else {
			cairo_tag_begin (cr, CAIRO_TAG_DEST, "name='second' x=20 y=40");
			cairo_tag_end (cr, CAIRO_TAG_DEST);
		}
		cairo_show_text (cr, i ? "Second page" : "First page");
		if (!i)
			cairo_tag_end (cr, CAIRO_TAG_LINK);
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

static PpsAnnotation *
find_annotation (GList *annotations, PpsAnnotationType type)
{
	for (GList *item = annotations; item; item = item->next)
		if (pps_annotation_get_annotation_type (item->data) == type)
			return item->data;
	g_error ("Missing annotation type %u", type);
}

static void
annotations_edit_and_save (void)
{
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, fixture_uri);
	PpsDocumentAnnotations *annotations = PPS_DOCUMENT_ANNOTATIONS (document);
	g_assert_true (pps_document_annotations_can_add_annotation (annotations));
	g_assert_true (pps_document_annotations_can_remove_annotation (annotations));
	g_autoptr (PpsPage) page = pps_document_get_page (document, 0);
	g_assert_null (pps_document_annotations_get_annotations (annotations, page));
	g_autoptr (PpsAnnotation) note = pps_annotation_text_new (page);
	PpsRectangle area = { 15, 15, 39, 39 };
	pps_annotation_set_area (note, &area);
	pps_annotation_markup_set_label (PPS_ANNOTATION_MARKUP (note), "Author α");
	pps_document_annotations_add_annotation (annotations, note);
	guint64 id = _pps_document_objects_get_id (_pps_document_proxy_get_objects (document), G_OBJECT (note));
	g_assert_cmpuint (id, !=, 0);
	g_assert_nonnull (pps_annotation_get_name (note));
	g_assert_true (pps_document_get_modified (document));
	g_assert_true (pps_document_annotations_document_is_modified (annotations));
	pps_annotation_set_contents (note, "Edited note α");
	pps_annotation_text_set_icon (PPS_ANNOTATION_TEXT (note), PPS_ANNOTATION_TEXT_ICON_COMMENT);
	GdkRGBA color = { .2, .4, .8, 1. };
	pps_annotation_set_rgba (note, &color);
	pps_annotation_markup_set_opacity (PPS_ANNOTATION_MARKUP (note), .6);
	g_autolist (PpsAnnotation) first_view = pps_document_annotations_get_annotations (annotations, page);
	g_assert_cmpuint (g_list_length (first_view), ==, 1);
	PpsAnnotation *copy = first_view->data;
	g_assert_true (copy != note);
	g_assert_cmpuint (_pps_document_objects_get_id (_pps_document_proxy_get_objects (document), G_OBJECT (copy)), ==, id);
	g_assert_cmpstr (pps_annotation_get_contents (copy), ==, "Edited note α");
	pps_annotation_set_contents (copy, "Second view");
	/* Independent wrappers retain per-view state; a fresh read sees the backend edit. */
	g_assert_cmpstr (pps_annotation_get_contents (note), ==, "Edited note α");
	g_autolist (PpsAnnotation) updated = pps_document_annotations_get_annotations (annotations, page);
	g_assert_cmpstr (pps_annotation_get_contents (updated->data), ==, "Second view");
	g_autoptr (PpsAnnotation) highlight = pps_annotation_text_markup_highlight_new (page);
	PpsRectangle text_area = { 10, 14, 100, 34 };
	pps_annotation_set_area (highlight, &text_area);
	pps_annotation_set_rgba (highlight, &color);
	pps_document_annotations_add_annotation (annotations, highlight);
	pps_annotation_set_contents (highlight, "Highlighted text");
	PpsRectangle normalized;
	pps_annotation_get_area (highlight, &normalized);
	g_assert_cmpint (pps_document_annotations_over_markup (annotations, highlight,
	                                                       (normalized.x1 + normalized.x2) / 2, (normalized.y1 + normalized.y2) / 2),
	                 ==, PPS_ANNOTATION_OVER_MARKUP_YES);
	g_assert_cmpint (pps_document_annotations_over_markup (annotations, highlight, 140, 280), ==, PPS_ANNOTATION_OVER_MARKUP_NOT);
	pps_document_annotations_remove_annotation (annotations, note);
	g_autolist (PpsAnnotation) removed = pps_document_annotations_get_annotations (annotations, page);
	g_assert_cmpuint (g_list_length (removed), ==, 1);
	pps_annotation_set_contents (note, "Edited while removed");
	pps_document_annotations_add_annotation (annotations, note);
	g_assert_cmpuint (_pps_document_objects_get_id (_pps_document_proxy_get_objects (document), G_OBJECT (note)), ==, id);
	pps_annotation_set_contents (note, "Redo note");
	pps_annotation_set_contents (copy, "Other view after redo");
	g_autofree char *saved_path = g_build_filename (fixture_dir, "annotations-saved.pdf", NULL);
	g_autofree char *saved_uri = g_filename_to_uri (saved_path, NULL, NULL);
	g_autoptr (GError) error = NULL;
	g_assert_true (pps_document_save (document, saved_uri, &error));
	g_assert_no_error (error);
	g_assert_false (pps_document_get_modified (document));
	g_assert_false (pps_document_annotations_document_is_modified (annotations));
	g_autoptr (PpsDocument) saved = new_document ();
	load_document (saved, saved_uri);
	g_autoptr (PpsPage) saved_page = pps_document_get_page (saved, 0);
	g_autolist (PpsAnnotation) saved_annotations = pps_document_annotations_get_annotations (PPS_DOCUMENT_ANNOTATIONS (saved), saved_page);
	g_assert_cmpuint (g_list_length (saved_annotations), ==, 2);
	g_assert_cmpstr (pps_annotation_get_contents (find_annotation (saved_annotations, PPS_ANNOTATION_TYPE_TEXT)), ==, "Other view after redo");
	g_assert_cmpstr (pps_annotation_get_contents (find_annotation (saved_annotations, PPS_ANNOTATION_TYPE_TEXT_MARKUP)), ==, "Highlighted text");
	render_page (saved, 0);
	g_assert_cmpint (g_unlink (saved_path), ==, 0);
}

static void
annotation_ink_stamp_and_fonts (void)
{
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, fixture_uri);
	PpsDocumentAnnotations *annotations = PPS_DOCUMENT_ANNOTATIONS (document);
	g_autoptr (PpsPage) page = pps_document_get_page (document, 0);
	PpsRectangle area = { 10, 115, 120, 150 };
	g_autoptr (PpsAnnotation) text = pps_annotation_free_text_new (page);
	g_autoptr (PangoFontDescription) font = pango_font_description_from_string ("Sans Bold 14");
	GdkRGBA color = { .2, .3, .8, 1. };
	pps_annotation_set_area (text, &area);
	pps_annotation_set_contents (text, "Free text");
	pps_annotation_free_text_set_font_description (PPS_ANNOTATION_FREE_TEXT (text), font);
	pps_annotation_free_text_set_font_rgba (PPS_ANNOTATION_FREE_TEXT (text), &color);
	pps_document_annotations_add_annotation (annotations, text);
	pps_annotation_set_contents (text, "Changed free text");
	pango_font_description_set_size (font, 18 * PANGO_SCALE);
	pps_annotation_free_text_set_font_description (PPS_ANNOTATION_FREE_TEXT (text), font);
	g_autoptr (PpsAnnotation) ink = pps_annotation_ink_new (page);
	PpsPoint points[] = { { 20, 160 }, { 40, 180 }, { 60, 160 } };
	PpsPath path = { points, 3 };
	pps_annotation_ink_set_ink_list (PPS_ANNOTATION_INK (ink), pps_ink_list_new_for_array ((const PpsPath *[]){ &path }, 1));
	pps_annotation_set_border_width (ink, 2);
	pps_annotation_set_rgba (ink, &color);
	pps_document_annotations_add_annotation (annotations, ink);
	points[1].y = 190;
	pps_annotation_ink_set_ink_list (PPS_ANNOTATION_INK (ink), pps_ink_list_new_for_array ((const PpsPath *[]){ &path }, 1));
	g_autoptr (PpsAnnotation) stamp = pps_annotation_stamp_new (page);
	cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 8, 8);
	cairo_t *cr = cairo_create (surface);
	cairo_set_source_rgb (cr, 1, 0, 0);
	cairo_paint (cr);
	cairo_destroy (cr);
	pps_annotation_stamp_set_surface (PPS_ANNOTATION_STAMP (stamp), surface);
	cairo_surface_destroy (surface);
	area = (PpsRectangle){ 80, 160, 120, 200 };
	pps_annotation_set_area (stamp, &area);
	pps_document_annotations_add_annotation (annotations, stamp);
	g_assert_cmphex (pixel_color (document, 100, 180), ==, 0xff0000);
	pps_document_annotations_remove_annotation (annotations, stamp);
	pps_document_annotations_add_annotation (annotations, stamp);
	g_assert_cmphex (pixel_color (document, 100, 180), ==, 0xff0000);
	g_autofree char *saved_path = g_build_filename (fixture_dir, "annotation-types.pdf", NULL);
	g_autofree char *saved_uri = g_filename_to_uri (saved_path, NULL, NULL);
	g_autoptr (GError) error = NULL;
	g_assert_true (pps_document_save (document, saved_uri, &error));
	g_assert_no_error (error);
	g_autoptr (PpsDocument) saved = new_document ();
	load_document (saved, saved_uri);
	g_autoptr (PpsPage) saved_page = pps_document_get_page (saved, 0);
	g_autolist (PpsAnnotation) list = pps_document_annotations_get_annotations (PPS_DOCUMENT_ANNOTATIONS (saved), saved_page);
	g_assert_cmpuint (g_list_length (list), ==, 3);
	PpsAnnotation *saved_text = find_annotation (list, PPS_ANNOTATION_TYPE_FREE_TEXT);
	g_assert_cmpstr (pps_annotation_get_contents (saved_text), ==, "Changed free text");
	g_autoptr (PangoFontDescription) saved_font = pps_annotation_free_text_get_font_description (PPS_ANNOTATION_FREE_TEXT (saved_text));
	g_assert_cmpint (pango_font_description_get_size (saved_font), ==, 18 * PANGO_SCALE);
	PpsInkList *saved_ink = pps_annotation_ink_get_ink_list (PPS_ANNOTATION_INK (find_annotation (list, PPS_ANNOTATION_TYPE_INK)));
	g_assert_cmpuint (saved_ink->n_paths, ==, 1);
	g_assert_cmpfloat (saved_ink->paths[0]->points[1].y, ==, 190);
	g_assert_nonnull (find_annotation (list, PPS_ANNOTATION_TYPE_STAMP));
	render_page (saved, 0);
	g_assert_cmphex (pixel_color (saved, 100, 180), ==, 0xff0000);
	g_assert_cmpint (g_unlink (saved_path), ==, 0);
}

static void
annotation_capabilities (void)
{
	const char *interfaces[] = { "PpsDocumentAnnotations", NULL };
	GType read_only = _pps_document_proxy_type_for_interfaces (interfaces, 0);
	GType editable = _pps_document_proxy_type_for_interfaces (interfaces, PPS_ANNOTATION_METHOD_ADD | PPS_ANNOTATION_METHOD_REMOVE);
	g_assert_cmpuint (read_only, !=, editable);
	g_autoptr (PpsDocument) read_document = g_object_new (read_only, NULL);
	g_autoptr (PpsDocument) edit_document = g_object_new (editable, NULL);
	g_assert_false (pps_document_annotations_can_add_annotation (PPS_DOCUMENT_ANNOTATIONS (read_document)));
	g_assert_false (pps_document_annotations_can_remove_annotation (PPS_DOCUMENT_ANNOTATIONS (read_document)));
	g_assert_true (pps_document_annotations_can_add_annotation (PPS_DOCUMENT_ANNOTATIONS (edit_document)));
	g_assert_true (pps_document_annotations_can_remove_annotation (PPS_DOCUMENT_ANNOTATIONS (edit_document)));
	g_assert_cmpint (pps_document_annotations_over_markup (PPS_DOCUMENT_ANNOTATIONS (read_document), NULL, 0, 0),
	                 ==, PPS_ANNOTATION_OVER_MARKUP_NOT_IMPLEMENTED);
}

static void
annotation_scope_and_errors (void)
{
	g_autoptr (PpsDocument) first = new_document ();
	g_autoptr (PpsDocument) second = new_document ();
	load_document (first, fixture_uri);
	load_document (second, fixture_uri);
	g_autoptr (PpsPage) page = pps_document_get_page (first, 0);
	g_autoptr (PpsAnnotation) note = pps_annotation_text_new (page);
	pps_annotation_set_area (note, &(PpsRectangle){ 10, 10, 34, 34 });
	pps_document_annotations_add_annotation (PPS_DOCUMENT_ANNOTATIONS (first), note);
	pps_annotation_set_contents (note, "First document");
	pps_document_annotations_add_annotation (PPS_DOCUMENT_ANNOTATIONS (second), note);
	pps_document_annotations_remove_annotation (PPS_DOCUMENT_ANNOTATIONS (second), note);
	g_assert_false (pps_document_get_modified (second));
	g_assert_null (pps_document_annotations_get_annotations (PPS_DOCUMENT_ANNOTATIONS (second), page));
	PpsDocumentSession *session = _pps_document_proxy_get_session (first);
	guint64 id = _pps_document_objects_get_id (_pps_document_proxy_get_objects (first), G_OBJECT (note));
	g_autoptr (GError) error = NULL;
	guint state;
	g_assert_false (pps_dbus_document_call_remove_annotation_sync (session->proxy, G_MAXUINT64,
	                                                               G_DBUS_CALL_FLAGS_NONE, 5000, &state, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	g_autoptr (GVariant) invalid = g_variant_ref_sink (g_variant_new_variant (g_variant_new_double (NAN)));
	g_assert_false (pps_dbus_document_call_set_annotation_property_sync (session->proxy, id, "border-width", invalid,
	                                                                     G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &state, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_clear_error (&error);
	guint hit;
	g_assert_false (pps_dbus_document_call_over_annotation_markup_sync (session->proxy, id, NAN, 0,
	                                                                    G_DBUS_CALL_FLAGS_NONE, 5000, &hit, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	g_autoptr (GVariant) values = NULL;
	guint64 missing_id = G_MAXUINT64;
	g_autoptr (GVariant) missing = g_variant_ref_sink (g_variant_new_fixed_array (G_VARIANT_TYPE_UINT64, &missing_id, 1, sizeof missing_id));
	g_assert_false (pps_dbus_document_call_get_annotations_sync (session->proxy, 0, missing,
	                                                             G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &values, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	pid_t pid = g_ascii_strtoll (g_subprocess_get_identifier (session->process), NULL, 10);
	g_subprocess_send_signal (session->process, SIGSTOP);
	siginfo_t status;
	g_assert_cmpint (waitid (P_PID, pid, &status, WSTOPPED | WNOWAIT), ==, 0);
	gboolean dirty = pps_document_annotations_document_is_modified (PPS_DOCUMENT_ANNOTATIONS (first));
	g_subprocess_send_signal (session->process, SIGCONT);
	g_assert_true (dirty);
	pps_document_annotations_remove_annotation (PPS_DOCUMENT_ANNOTATIONS (first), note);
	g_assert_false (pps_dbus_document_call_remove_annotation_sync (session->proxy, id,
	                                                               G_DBUS_CALL_FLAGS_NONE, 5000, &state, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	pps_document_annotations_add_annotation (PPS_DOCUMENT_ANNOTATIONS (first), note);
	g_autolist (PpsAnnotation) list = pps_document_annotations_get_annotations (PPS_DOCUMENT_ANNOTATIONS (first), page);
	g_assert_cmpuint (g_list_length (list), ==, 1);
	render_page (second, 0);
	/* A surviving wrapper's weak document connection must be harmless. */
	g_clear_object (&first);
	pps_annotation_set_contents (note, "Document closed");
}

static void
annotation_attachment_batches (void)
{
	const guint count = 33;
	g_autoptr (GString) references = g_string_new (NULL);
	for (guint i = 0; i < count; i++)
		g_string_append_printf (references, "%u 0 R ", 7 + i);
	g_autoptr (GPtrArray) objects = g_ptr_array_new_with_free_func (g_free);
	g_ptr_array_add (objects, g_strdup ("<< /Type /Catalog /Pages 2 0 R >>"));
	g_ptr_array_add (objects, g_strdup ("<< /Type /Pages /Count 1 /Kids [3 0 R] >>"));
	g_ptr_array_add (objects, g_strdup_printf ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> /Contents 4 0 R /Annots [%s] >>", references->str));
	g_ptr_array_add (objects, g_strdup ("<< /Length 0 >>\nstream\nendstream"));
	g_ptr_array_add (objects, g_strdup ("<< /Type /EmbeddedFile /Filter /ASCIIHexDecode /Length 5 >>\nstream\n6162>\nendstream"));
	g_ptr_array_add (objects, g_strdup ("<< /Type /Filespec /F (embedded.txt) /Desc (Annotation payload) /EF << /F 5 0 R >> >>"));
	for (guint i = 0; i < count; i++)
		g_ptr_array_add (objects, g_strdup_printf ("<< /Type /Annot /Subtype /FileAttachment /Rect [10 10 30 30] /FS 6 0 R /NM (attachment%u) /Contents (Attached) >>", i));
	g_autofree char *path = write_pdf_fixture ("attachment-annotations.pdf", (const char *const *) objects->pdata, objects->len);
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, uri);
	g_autoptr (PpsPage) page = pps_document_get_page (document, 0);
	g_autolist (PpsAnnotation) annotations = pps_document_annotations_get_annotations (PPS_DOCUMENT_ANNOTATIONS (document), page);
	g_assert_cmpuint (g_list_length (annotations), ==, count);
	g_autolist (PpsAnnotation) again = pps_document_annotations_get_annotations (PPS_DOCUMENT_ANNOTATIONS (document), page);
	g_assert_cmpuint (g_list_length (again), ==, count);
	g_assert_true (annotations->data != again->data);
	g_assert_cmpuint (_pps_document_objects_get_id (_pps_document_proxy_get_objects (document), annotations->data), ==,
	                  _pps_document_objects_get_id (_pps_document_proxy_get_objects (document), again->data));
	PpsDocumentSession *session = _pps_document_proxy_get_session (document);
	g_autoptr (GError) error = NULL;
	g_autoptr (GVariant) handles = NULL;
	g_assert_true (pps_dbus_document_call_list_annotations_sync (session->proxy, 0,
	                                                             G_DBUS_CALL_FLAGS_NONE, 5000, &handles, NULL, &error));
	g_assert_no_error (error);
	g_assert_cmpuint (g_variant_n_children (handles), ==, count);
	for (guint offset = 0; offset < count; offset += PPS_DOCUMENT_BUFFER_BATCH_SIZE) {
		/* Membership changes between batches must not shift the original handle list. */
		if (offset == PPS_DOCUMENT_BUFFER_BATCH_SIZE) {
			pps_document_annotations_remove_annotation (PPS_DOCUMENT_ANNOTATIONS (document), g_list_nth_data (annotations, offset));
			g_autoptr (PpsAnnotation) note = pps_annotation_text_new (page);
			pps_annotation_set_area (note, &(PpsRectangle){ 10, 10, 34, 34 });
			pps_document_annotations_add_annotation (PPS_DOCUMENT_ANNOTATIONS (document), note);
		}
		guint batch_size = MIN (PPS_DOCUMENT_BUFFER_BATCH_SIZE, count - offset);
		GVariantBuilder builder;
		g_variant_builder_init (&builder, G_VARIANT_TYPE ("at"));
		for (guint i = 0; i < batch_size; i++) {
			guint64 id;
			g_variant_get_child (handles, offset + i, "t", &id);
			g_variant_builder_add (&builder, "t", id);
		}
		g_autoptr (GVariant) batch = g_variant_ref_sink (g_variant_builder_end (&builder));
		g_autoptr (GVariant) values = NULL;
		g_autoptr (GUnixFDList) descriptors = NULL;
		g_assert_true (pps_dbus_document_call_get_annotations_sync (session->proxy, 0, batch,
		                                                            G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &values, &descriptors, NULL, &error));
		g_assert_no_error (error);
		g_assert_cmpuint (g_variant_n_children (values), ==, batch_size);
		g_assert_cmpint (g_unix_fd_list_get_length (descriptors), ==, batch_size);
		for (guint i = 0; i < batch_size; i++) {
			guint64 id, expected;
			g_autoptr (GVariant) value = NULL;
			g_variant_get_child (values, i, "(t@a{sv})", &id, &value);
			g_variant_get_child (handles, offset + i, "t", &expected);
			g_assert_cmpuint (id, ==, expected);
			g_autoptr (PpsAnnotation) annotation = _pps_annotation_from_variant (value, page, descriptors, &error);
			g_assert_no_error (error);
			g_assert_true (PPS_IS_ANNOTATION_ATTACHMENT (annotation));
			g_autofree char *name = g_strdup_printf ("attachment%u", offset + i);
			g_assert_cmpstr (pps_annotation_get_name (annotation), ==, name);
		}
	}
	g_autolist (PpsAnnotation) changed = pps_document_annotations_get_annotations (PPS_DOCUMENT_ANNOTATIONS (document), page);
	g_assert_cmpuint (g_list_length (changed), ==, count);
	g_assert_true (PPS_IS_ANNOTATION_TEXT (g_list_last (changed)->data));
	g_clear_object (&document);
	for (GList *item = annotations; item; item = item->next) {
		g_assert_true (PPS_IS_ANNOTATION_ATTACHMENT (item->data));
		PpsAttachment *attachment = pps_annotation_attachment_get_attachment (PPS_ANNOTATION_ATTACHMENT (item->data));
		g_assert_cmpstr (pps_attachment_get_name (attachment), ==, "embedded.txt");
		g_autoptr (GBytes) bytes = _pps_attachment_get_data (attachment);
		g_assert_cmpmem (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes), "ab", 2);
	}
	g_assert_cmpint (g_unlink (path), ==, 0);
}

static void
font_information (void)
{
	const char *content = "BT /F1 12 Tf 10 60 Td (Standard) Tj /F2 12 Tf 0 -20 Td (Substitute) Tj ET\n";
	g_autofree char *stream = g_strdup_printf ("<< /Length %zu >>\nstream\n%sendstream", strlen (content), content);
	const char *objects[] = {
		"<< /Type /Catalog /Pages 2 0 R >>",
		"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
		"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> /Contents 4 0 R >>",
		stream,
		"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
		"<< /Type /Font /Subtype /Type1 /BaseFont /UnknownFont#FF >>",
	};
	g_autofree char *path = write_pdf_fixture ("fonts.pdf", objects, G_N_ELEMENTS (objects));
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, uri);
	g_assert_true (PPS_IS_DOCUMENT_FONTS (document));
	PpsDocumentFonts *fonts = PPS_DOCUMENT_FONTS (document);
	g_assert_null (pps_document_fonts_get_model (fonts));
	g_autofree char *before = g_strdup (pps_document_fonts_get_fonts_summary (fonts));
	g_assert_nonnull (before);
	pps_document_fonts_scan (fonts);
	g_autoptr (GListModel) model = pps_document_fonts_get_model (fonts);
	g_assert_nonnull (model);
	g_assert_cmpuint (g_list_model_get_n_items (model), ==, 2);
	g_assert_cmpuint (g_list_model_get_item_type (model), ==, PPS_TYPE_FONT_DESCRIPTION);
	g_assert_nonnull (pps_document_fonts_get_fonts_summary (fonts));
	g_assert_cmpstr (pps_document_fonts_get_fonts_summary (fonts), !=, before);
	/* A fresh scan must still produce the complete list. */
	pps_document_fonts_scan (fonts);
	g_autoptr (GListModel) again = pps_document_fonts_get_model (fonts);
	g_assert_cmpuint (g_list_model_get_n_items (again), ==, 2);
	g_clear_object (&document);
	gboolean standard = FALSE, substitute = FALSE;
	for (guint i = 0; i < g_list_model_get_n_items (model); i++) {
		g_autoptr (GObject) font = g_list_model_get_item (model, i);
		g_autofree char *name = NULL, *details = NULL;
		g_object_get (font, "name", &name, "details", &details, NULL);
		g_assert_nonnull (name);
		g_assert_true (g_utf8_validate (name, -1, NULL));
		g_assert_nonnull (details);
		g_assert_true (g_utf8_validate (details, -1, NULL));
		g_assert_cmpstr (details, !=, "");
		standard |= g_str_equal (name, "Helvetica");
		substitute |= g_str_equal (name, "UnknownFont\357\277\275");
	}
	g_assert_true (standard);
	g_assert_true (substitute);
	g_assert_cmpint (g_unlink (path), ==, 0);
}

static void
no_fonts (void)
{
	const char *objects[] = {
		"<< /Type /Catalog /Pages 2 0 R >>",
		"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
		"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> /Contents 4 0 R >>",
		"<< /Length 0 >>\nstream\nendstream",
	};
	g_autofree char *path = write_pdf_fixture ("no-fonts.pdf", objects, G_N_ELEMENTS (objects));
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, uri);
	PpsDocumentFonts *fonts = PPS_DOCUMENT_FONTS (document);
	pps_document_fonts_scan (fonts);
	g_assert_null (pps_document_fonts_get_model (fonts));
	g_assert_nonnull (pps_document_fonts_get_fonts_summary (fonts));
	render_page (document, 0);
	g_assert_cmpint (g_unlink (path), ==, 0);
}

static void
embedded_images (void)
{
	const char *content = "q 20 0 0 10 5 20 cm /Im1 Do Q q 20 0 0 10 40 20 cm /Im2 Do Q\n";
	g_autofree char *stream = g_strdup_printf ("<< /Length %zu >>\nstream\n%sendstream", strlen (content), content);
	const char *objects[] = {
		"<< /Type /Catalog /Pages 2 0 R >>",
		"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
		"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << /XObject << /Im1 5 0 R /Im2 6 0 R >> >> /Contents 4 0 R >>",
		stream,
		"<< /Type /XObject /Subtype /Image /Width 2 /Height 1 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /ASCIIHexDecode /Length 13 >>\nstream\nFF00000000FF>\nendstream",
		"<< /Type /XObject /Subtype /Image /Width 2 /Height 1 /ColorSpace /DeviceRGB /BitsPerComponent 8 /SMask 7 0 R /Filter /ASCIIHexDecode /Length 13 >>\nstream\n00FF00FFFFFF>\nendstream",
		"<< /Type /XObject /Subtype /Image /Width 2 /Height 1 /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /ASCIIHexDecode /Length 5 >>\nstream\n80FF>\nendstream",
	};
	g_autofree char *path = write_pdf_fixture ("images.pdf", objects, G_N_ELEMENTS (objects));
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	g_autoptr (PpsDocument) first = new_document ();
	g_autoptr (PpsDocument) second = new_document ();
	load_document (first, uri);
	load_document (second, uri);
	g_assert_true (PPS_IS_DOCUMENT_IMAGES (first));
	PpsDocumentImages *images = PPS_DOCUMENT_IMAGES (first);
	g_autoptr (PpsPage) page = pps_document_get_page (first, 0);
	PpsMappingList *mappings = pps_document_images_get_image_mapping (images, page);
	PpsMappingList *again = pps_document_images_get_image_mapping (images, page);
	g_assert_cmpuint (pps_mapping_list_length (mappings), ==, 2);
	g_assert_cmpuint (pps_mapping_list_length (again), ==, 2);
	g_autoptr (GdkPixbuf) retained = NULL;
	for (guint i = 0; i < pps_mapping_list_length (mappings); i++) {
		PpsMapping *mapping = pps_mapping_list_nth (mappings, i);
		PpsMapping *other = pps_mapping_list_nth (again, i);
		g_assert_true (mapping->data != other->data);
		g_assert_cmpuint (_pps_document_objects_get_id (_pps_document_proxy_get_objects (first), mapping->data), ==,
		                  _pps_document_objects_get_id (_pps_document_proxy_get_objects (first), other->data));
		g_assert_cmpint (pps_image_get_page (mapping->data), ==, 0);
		g_assert_cmpint (pps_image_get_id (mapping->data), ==, pps_image_get_id (other->data));
		g_assert_cmpfloat (mapping->area.x2 - mapping->area.x1, ==, 20);
		g_assert_cmpfloat (mapping->area.y2 - mapping->area.y1, ==, 10);
		g_autoptr (GdkPixbuf) pixels = pps_document_images_get_image (images, mapping->data);
		g_assert_nonnull (pixels);
		g_assert_cmpint (gdk_pixbuf_get_width (pixels), ==, 2);
		g_assert_cmpint (gdk_pixbuf_get_height (pixels), ==, 1);
		const guint8 *data = gdk_pixbuf_read_pixels (pixels);
		if (mapping->area.x1 == 5) {
			g_assert_cmpmem (data, 3, "\377\0\0", 3);
			g_assert_cmpmem (data + gdk_pixbuf_get_n_channels (pixels), 3, "\0\0\377", 3);
		} else {
			g_assert_cmpfloat (mapping->area.x1, ==, 40);
			g_assert_true (gdk_pixbuf_get_has_alpha (pixels));
			g_assert_cmpmem (data, 8, "\0\377\0\200\377\377\377\377", 8);
		}
		gdk_pixbuf_get_pixels (pixels)[0] ^= 0xff;
		g_autoptr (GdkPixbuf) unchanged = pps_document_images_get_image (images, other->data);
		g_assert_nonnull (unchanged);
		g_assert_cmpint (gdk_pixbuf_read_pixels (pixels)[0], !=, gdk_pixbuf_read_pixels (unchanged)[0]);
		g_set_object (&retained, unchanged);
		g_assert_null (pps_document_images_get_image (PPS_DOCUMENT_IMAGES (second), mapping->data));
	}
	g_autoptr (PpsImage) unbound = pps_image_new (0, 0);
	g_assert_null (pps_document_images_get_image (images, unbound));
	PpsDocumentSession *session = _pps_document_proxy_get_session (first);
	g_autoptr (GError) error = NULL;
	g_assert_false (pps_dbus_document_call_get_image_sync (session->proxy, G_MAXUINT64,
	                                                       G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	for (int index = -1; index <= 1; index += 2) {
		g_assert_false (pps_dbus_document_call_get_image_mapping_sync (session->proxy, index,
		                                                               G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL, &error));
		g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
		g_clear_error (&error);
	}
	render_page (first, 0);
	g_clear_object (&first);
	g_autofree char *saved = g_build_filename (fixture_dir, "image.png", NULL);
	g_assert_true (gdk_pixbuf_save (retained, saved, "png", &error, NULL));
	g_assert_no_error (error);
	g_autoptr (GdkPixbuf) reopened = gdk_pixbuf_new_from_file (saved, &error);
	g_assert_no_error (error);
	g_assert_cmpmem (gdk_pixbuf_read_pixels (reopened), gdk_pixbuf_get_byte_length (reopened),
	                 gdk_pixbuf_read_pixels (retained), gdk_pixbuf_get_byte_length (retained));
	PpsMappingList *independent = pps_document_images_get_image_mapping (PPS_DOCUMENT_IMAGES (second), page);
	g_autoptr (GdkPixbuf) valid = pps_document_images_get_image (PPS_DOCUMENT_IMAGES (second), pps_mapping_list_nth (independent, 0)->data);
	g_assert_nonnull (valid);
	pps_mapping_list_unref (independent);
	pps_mapping_list_unref (again);
	pps_mapping_list_unref (mappings);
	g_assert_cmpint (g_unlink (path), ==, 0);
	g_assert_cmpint (g_unlink (saved), ==, 0);
}

static void
no_images (void)
{
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, fixture_uri);
	g_autoptr (PpsPage) page = pps_document_get_page (document, 0);
	PpsMappingList *mappings = pps_document_images_get_image_mapping (PPS_DOCUMENT_IMAGES (document), page);
	g_assert_nonnull (mappings);
	g_assert_cmpuint (pps_mapping_list_length (mappings), ==, 0);
	pps_mapping_list_unref (mappings);
}

static void
presentation_transitions (void)
{
	const char *names[] = { "R", "Split", "Blinds", "Box", "Wipe", "Dissolve", "Glitter", "Fly", "Push", "Cover", "Uncover", "Fade" };
	guint count = G_N_ELEMENTS (names) + 1;
	g_autoptr (GString) references = g_string_new (NULL);
	for (guint i = 0; i < count; i++)
		g_string_append_printf (references, "%u 0 R ", i + 3);
	g_autoptr (GPtrArray) objects = g_ptr_array_new_with_free_func (g_free);
	g_ptr_array_add (objects, g_strdup ("<< /Type /Catalog /Pages 2 0 R >>"));
	g_ptr_array_add (objects, g_strdup_printf ("<< /Type /Pages /Count %u /Kids [%s] >>", count, references->str));
	for (guint i = 0; i < count; i++) {
		g_autofree char *transition = i < G_N_ELEMENTS (names) ? g_strdup_printf ("/Dur 4.25 /Trans << /S /%s /D 2.75 /Dm /V /M /O /Di 270 /SS 0.625 /B true >>", names[i]) : g_strdup ("");
		g_ptr_array_add (objects, g_strdup_printf ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> /Contents %u 0 R %s >>", count + 3, transition));
	}
	g_ptr_array_add (objects, g_strdup ("<< /Length 0 >>\nstream\nendstream"));
	g_autofree char *path = write_pdf_fixture ("transitions.pdf", (const char *const *) objects->pdata, objects->len);
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, uri);
	g_assert_true (PPS_IS_DOCUMENT_TRANSITION (document));
	PpsDocumentTransition *transitions = PPS_DOCUMENT_TRANSITION (document);
	for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
		g_assert_cmpfloat (pps_document_transition_get_page_duration (transitions, i), ==, 4.25);
		g_autoptr (PpsTransitionEffect) effect = pps_document_transition_get_effect (transitions, i);
		int type;
		double duration;
		g_object_get (effect, "type", &type, "duration-real", &duration, NULL);
		g_assert_cmpint (type, ==, i);
		g_assert_cmpfloat (duration, ==, 2.75);
	}
	g_autoptr (PpsTransitionEffect) fly = pps_document_transition_get_effect (transitions, PPS_TRANSITION_EFFECT_FLY);
	int alignment, direction, angle;
	double scale;
	gboolean rectangular;
	g_object_get (fly, "alignment", &alignment, "direction", &direction, "angle", &angle,
	              "scale", &scale, "rectangular", &rectangular, NULL);
	g_assert_cmpint (alignment, ==, PPS_TRANSITION_ALIGNMENT_VERTICAL);
	g_assert_cmpint (direction, ==, PPS_TRANSITION_DIRECTION_OUTWARD);
	g_assert_cmpint (angle, ==, 270);
	g_assert_cmpfloat_with_epsilon (scale, .625, 1e-12);
	g_assert_true (rectangular);
	g_assert_cmpfloat (pps_document_transition_get_page_duration (transitions, count - 1), ==, -1);
	g_autoptr (PpsTransitionEffect) none = pps_document_transition_get_effect (transitions, count - 1);
	int type;
	g_object_get (none, "type", &type, NULL);
	g_assert_cmpint (type, ==, PPS_TRANSITION_EFFECT_REPLACE);
	PpsDocumentSession *session = _pps_document_proxy_get_session (document);
	g_autoptr (GError) error = NULL;
	g_assert_false (pps_dbus_document_call_get_page_duration_sync (session->proxy, -1,
	                                                               G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	g_assert_false (pps_dbus_document_call_get_transition_effect_sync (session->proxy, count,
	                                                                   G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_assert_cmpfloat (pps_document_transition_get_page_duration (transitions, count), ==, -1);
	render_page (document, count - 1);
	g_clear_object (&document);
	g_object_get (fly, "scale", &scale, "rectangular", &rectangular, NULL);
	g_assert_cmpfloat_with_epsilon (scale, .625, 1e-12);
	g_assert_true (rectangular);
	g_assert_cmpint (g_unlink (path), ==, 0);
}

static void
close_media_worker (PpsDocument *document)
{
	PpsDocumentSession *session = _pps_document_proxy_get_session (document);
	g_autofree char *name = g_strdup_printf ("papers-document-worker-%s", g_subprocess_get_identifier (session->process));
	g_autofree char *directory = g_build_filename (g_get_tmp_dir (), name, NULL);
	g_autoptr (GError) error = NULL;
	g_assert_true (g_file_test (directory, G_FILE_TEST_IS_DIR));
	g_assert_true (g_dbus_connection_close_sync (session->connection, NULL, &error));
	g_assert_no_error (error);
	g_assert_true (g_subprocess_wait_check (session->process, NULL, &error));
	g_assert_no_error (error);
	g_assert_false (g_file_test (directory, G_FILE_TEST_EXISTS));
}

static void
embedded_and_external_media (void)
{
	const char *objects[] = {
		"<< /Type /Catalog /Pages 2 0 R >>",
		"<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
		"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> /Contents 4 0 R /Annots [5 0 R 6 0 R 7 0 R 8 0 R] >>",
		"<< /Length 0 >>\nstream\nendstream",
		"<< /Type /Annot /Subtype /Movie /Rect [10 10 20 30] /Movie << /F (clip.ogg) >> /A << /ShowControls true >> >>",
		"<< /Type /Annot /Subtype /Movie /Rect [30 10 40 30] /Movie << /F (https://example.invalid/video.ogg) >> >>",
		"<< /Type /Annot /Subtype /Screen /Rect [50 10 60 30] /P 3 0 R /A << /S /Rendition /AN 7 0 R /OP 0 /R << /S /MR /C << /S /MCD /CT (audio/ogg) /D << /F (embedded.ogg) /EF << /F 9 0 R >> >> >> >> >> >>",
		"<< /Type /Annot /Subtype /Screen /Rect [70 10 80 30] /P 3 0 R /A << /S /Rendition /AN 8 0 R /OP 0 /R << /S /MR /C << /S /MCD /CT (audio/ogg) /D << /F (empty.ogg) /EF << /F 10 0 R >> >> >> >> >> >>",
		"<< /Type /EmbeddedFile /Filter /ASCIIHexDecode /Length 15 >>\nstream\n00FF6D65646961>\nendstream",
		"<< /Type /EmbeddedFile /Length 0 >>\nstream\nendstream",
	};
	g_autofree char *path = write_pdf_fixture ("media.pdf", objects, G_N_ELEMENTS (objects));
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, uri);
	g_assert_true (PPS_IS_DOCUMENT_MEDIA (document));
	g_autoptr (PpsPage) page = pps_document_get_page (document, 0);
	PpsMappingList *mappings = pps_document_media_get_media_mapping (PPS_DOCUMENT_MEDIA (document), page);
	PpsMappingList *again = pps_document_media_get_media_mapping (PPS_DOCUMENT_MEDIA (document), page);
	g_assert_nonnull (mappings);
	g_assert_cmpuint (pps_mapping_list_length (mappings), ==, 4);
	g_assert_cmpuint (pps_mapping_list_length (again), ==, 4);
	g_autofree char *relative_path = g_build_filename (fixture_dir, "clip.ogg", NULL);
	g_autofree char *relative_uri = g_filename_to_uri (relative_path, NULL, NULL);
	g_autoptr (PpsMedia) retained = NULL;
	g_autofree char *asset_path = NULL;
	g_autoptr (GError) error = NULL;
	for (guint i = 0; i < 4; i++) {
		PpsMapping *mapping = pps_mapping_list_nth (mappings, i);
		PpsMedia *media = mapping->data;
		PpsMedia *other = pps_mapping_list_nth (again, i)->data;
		g_assert_true (media != other);
		g_assert_cmpuint (pps_media_get_page_index (media), ==, 0);
		g_assert_cmpfloat (mapping->area.x2 - mapping->area.x1, ==, 10);
		g_assert_cmpfloat (mapping->area.y1, ==, 70);
		g_assert_cmpfloat (mapping->area.y2, ==, 90);
		g_assert_cmpstr (pps_media_get_uri (media), ==, pps_media_get_uri (other));
		if (mapping->area.x1 == 10) {
			g_assert_cmpstr (pps_media_get_uri (media), ==, relative_uri);
			g_assert_true (pps_media_get_show_controls (media));
			g_assert_null (_pps_media_get_temp_file (media));
		} else if (mapping->area.x1 == 30) {
			g_assert_cmpstr (pps_media_get_uri (media), ==, "https://example.invalid/video.ogg");
			g_assert_false (pps_media_get_show_controls (media));
			g_assert_null (_pps_media_get_temp_file (media));
		} else {
			g_assert_true (pps_media_get_show_controls (media));
			GFile *file = _pps_media_get_temp_file (media);
			g_assert_true (file == _pps_media_get_temp_file (other));
			g_autoptr (GBytes) bytes = g_file_load_bytes (file, NULL, NULL, &error);
			g_assert_no_error (error);
			if (mapping->area.x1 == 50) {
				g_assert_cmpmem (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes), "\0\377media", 7);
				g_set_object (&retained, media);
				asset_path = g_file_get_path (file);
			} else {
				g_assert_cmpfloat (mapping->area.x1, ==, 70);
				g_assert_cmpuint (g_bytes_get_size (bytes), ==, 0);
			}
		}
		gboolean controls = pps_media_get_show_controls (media);
		pps_media_set_show_controls (media, !controls);
		g_assert_cmpint (pps_media_get_show_controls (other), ==, controls);
	}
	PpsDocumentSession *session = _pps_document_proxy_get_session (document);
	g_assert_false (pps_dbus_document_call_get_media_data_sync (session->proxy, G_MAXUINT64,
	                                                            G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL, NULL, NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error (&error);
	g_autoptr (GVariant) values = NULL;
	g_assert_true (pps_dbus_document_call_get_media_mapping_sync (session->proxy, 0,
	                                                              G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &values, NULL, &error));
	g_assert_no_error (error);
	gboolean checked_external = FALSE;
	for (gsize i = 0; i < g_variant_n_children (values); i++) {
		g_autoptr (GVariant) item = g_variant_get_child_value (values, i);
		g_autoptr (GVariant) value = g_variant_get_child_value (item, 1);
		gboolean embedded;
		g_assert_true (g_variant_lookup (value, "embedded", "b", &embedded));
		if (!embedded) {
			guint64 handle;
			g_assert_true (g_variant_lookup (value, "handle", "t", &handle));
			g_assert_false (pps_dbus_document_call_get_media_data_sync (session->proxy, handle,
			                                                            G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL, NULL, NULL, &error));
			g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
			g_clear_error (&error);
			checked_external = TRUE;
		}
	}
	g_assert_true (checked_external);
	close_media_worker (document);
	g_clear_object (&document);
	pps_mapping_list_unref (mappings);
	pps_mapping_list_unref (again);
	g_assert_true (g_file_test (asset_path, G_FILE_TEST_EXISTS));
	g_autoptr (GBytes) bytes = g_file_load_bytes (_pps_media_get_temp_file (retained), NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpmem (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes), "\0\377media", 7);
	g_clear_object (&retained);
	g_assert_false (g_file_test (asset_path, G_FILE_TEST_EXISTS));
	g_assert_cmpint (g_unlink (path), ==, 0);
}

static void
many_embedded_media (void)
{
	const guint count = 33;
	g_autoptr (GString) references = g_string_new (NULL);
	for (guint i = 0; i < count; i++)
		g_string_append_printf (references, "%u 0 R ", i + 6);
	g_autoptr (GPtrArray) objects = g_ptr_array_new_with_free_func (g_free);
	g_ptr_array_add (objects, g_strdup ("<< /Type /Catalog /Pages 2 0 R >>"));
	g_ptr_array_add (objects, g_strdup ("<< /Type /Pages /Count 1 /Kids [3 0 R] >>"));
	g_ptr_array_add (objects, g_strdup_printf ("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << >> /Contents 4 0 R /Annots [%s] >>", references->str));
	g_ptr_array_add (objects, g_strdup ("<< /Length 0 >>\nstream\nendstream"));
	g_ptr_array_add (objects, g_strdup ("<< /Type /EmbeddedFile /Length 5 >>\nstream\nmedia\nendstream"));
	for (guint i = 0; i < count; i++)
		g_ptr_array_add (objects, g_strdup_printf ("<< /Type /Annot /Subtype /Screen /Rect [10 10 30 30] /P 3 0 R /A << /S /Rendition /AN %u 0 R /OP 0 /R << /S /MR /C << /S /MCD /CT (audio/ogg) /D << /F (embedded.ogg) /EF << /F 5 0 R >> >> >> >> >> >>", i + 6));
	g_autofree char *path = write_pdf_fixture ("many-media.pdf", (const char *const *) objects->pdata, objects->len);
	g_autofree char *uri = g_filename_to_uri (path, NULL, NULL);
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, uri);
	g_autoptr (PpsPage) page = pps_document_get_page (document, 0);
	PpsMappingList *mappings = pps_document_media_get_media_mapping (PPS_DOCUMENT_MEDIA (document), page);
	g_assert_nonnull (mappings);
	g_assert_cmpuint (pps_mapping_list_length (mappings), ==, count);
	for (guint i = 0; i < count; i++) {
		PpsMedia *media = pps_mapping_list_nth (mappings, i)->data;
		g_autoptr (GError) error = NULL;
		g_autoptr (GBytes) bytes = g_file_load_bytes (_pps_media_get_temp_file (media), NULL, NULL, &error);
		g_assert_no_error (error);
		g_assert_cmpmem (g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes), "media", 5);
	}
	close_media_worker (document);
	g_clear_object (&document);
	pps_mapping_list_unref (mappings);
	g_assert_cmpint (g_unlink (path), ==, 0);
}

static void
no_media (void)
{
	g_autoptr (PpsDocument) document = new_document ();
	load_document (document, fixture_uri);
	g_autoptr (PpsPage) page = pps_document_get_page (document, 0);
	g_assert_null (pps_document_media_get_media_mapping (PPS_DOCUMENT_MEDIA (document), page));
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_assert_true (pps_init ());
	create_fixture ();
	g_test_add_func ("/document-proxy/metadata-and-rendering", metadata_and_rendering);
	g_test_add_func ("/document-proxy/password-retry", password_retry);
	g_test_add_func ("/document-proxy/fd-and-save", fd_and_save);
	g_test_add_func ("/document-proxy/failure-and-lifetime", failure_and_lifetime);
	g_test_add_func ("/document-proxy/text-and-find", text_and_find);
	g_test_add_func ("/document-proxy/selection", selection);
	g_test_add_func ("/document-proxy/links-and-outlines", links_and_outlines);
	g_test_add_func ("/document-proxy/layers-and-actions", layers_and_layer_actions);
	g_test_add_func ("/document-proxy/forms-edit-and-save", forms_edit_and_save);
	g_test_add_func ("/document-proxy/forms-isolation-and-errors", forms_isolation_and_errors);
	g_test_add_func ("/document-proxy/embedded-attachments", embedded_attachments);
	g_test_add_func ("/document-proxy/attachment-batches", attachment_batches);
	g_test_add_func ("/document-proxy/annotations-edit-and-save", annotations_edit_and_save);
	g_test_add_func ("/document-proxy/annotation-ink-stamp-and-fonts", annotation_ink_stamp_and_fonts);
	g_test_add_func ("/document-proxy/annotation-capabilities", annotation_capabilities);
	g_test_add_func ("/document-proxy/annotation-scope-and-errors", annotation_scope_and_errors);
	g_test_add_func ("/document-proxy/annotation-attachment-batches", annotation_attachment_batches);
	g_test_add_func ("/document-proxy/font-information", font_information);
	g_test_add_func ("/document-proxy/no-fonts", no_fonts);
	g_test_add_func ("/document-proxy/embedded-images", embedded_images);
	g_test_add_func ("/document-proxy/no-images", no_images);
	g_test_add_func ("/document-proxy/presentation-transitions", presentation_transitions);
	g_test_add_func ("/document-proxy/embedded-and-external-media", embedded_and_external_media);
	g_test_add_func ("/document-proxy/many-embedded-media", many_embedded_media);
	g_test_add_func ("/document-proxy/no-media", no_media);
	int result = g_test_run ();
	g_unlink (fixture_path);
	g_rmdir (fixture_dir);
	g_free (fixture_path);
	g_free (fixture_dir);
	g_free (fixture_uri);
	pps_shutdown ();
	return result;
}

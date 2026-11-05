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
	g_autoptr (GString) pdf = g_string_new ("%PDF-1.7\n");
	gsize offsets[G_N_ELEMENTS (objects)];
	for (guint i = 0; i < G_N_ELEMENTS (objects); i++) {
		offsets[i] = pdf->len;
		g_string_append_printf (pdf, "%u 0 obj\n%s\nendobj\n", i + 1, objects[i]);
	}
	gsize xref = pdf->len;
	g_string_append_printf (pdf, "xref\n0 %u\n0000000000 65535 f \n", (guint) G_N_ELEMENTS (objects) + 1);
	for (guint i = 0; i < G_N_ELEMENTS (objects); i++)
		g_string_append_printf (pdf, "%010" G_GSIZE_FORMAT " 00000 n \n", offsets[i]);
	g_string_append_printf (pdf, "trailer\n<< /Size %u /Root 1 0 R >>\nstartxref\n%" G_GSIZE_FORMAT "\n%%%%EOF\n",
	                        (guint) G_N_ELEMENTS (objects) + 1, xref);
	g_autofree char *path = g_build_filename (fixture_dir, "layers.pdf", NULL);
	g_autoptr (GError) error = NULL;
	g_assert_true (g_file_set_contents (path, pdf->str, pdf->len, &error));
	g_assert_no_error (error);
	return g_steal_pointer (&path);
}

static guint32
page_color (PpsDocument *document)
{
	g_autoptr (PpsPage) page = pps_document_get_page (document, 0);
	g_autoptr (PpsRenderContext) context = pps_render_context_new (page, 0, 1, PPS_RENDER_ANNOTS_ALL);
	cairo_surface_t *surface = pps_document_render (document, context);
	g_assert_nonnull (surface);
	cairo_surface_flush (surface);
	guint32 pixel;
	memcpy (&pixel, cairo_image_surface_get_data (surface) + 50 * cairo_image_surface_get_stride (surface) + 50 * 4, 4);
	cairo_surface_destroy (surface);
	return pixel & 0xffffff;
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
	g_test_add_func ("/document-proxy/selection", selection);
	g_test_add_func ("/document-proxy/links-and-outlines", links_and_outlines);
	g_test_add_func ("/document-proxy/layers-and-actions", layers_and_layer_actions);
	int result = g_test_run ();
	g_unlink (fixture_path);
	g_rmdir (fixture_dir);
	g_free (fixture_path);
	g_free (fixture_dir);
	g_free (fixture_uri);
	return result;
}

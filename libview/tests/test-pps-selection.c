// SPDX-License-Identifier: GPL-2.0-or-later
#include "pps-view-private.h"
#include <gtk/gtk.h>
#include <papers-view.h>

typedef struct {
	PpsDocument parent;
	PpsView *view;
	guint calls;
} SelectionDocument;
typedef PpsDocumentClass SelectionDocumentClass;
static GType selection_document_get_type (void);
static void selection_iface_init (PpsSelectionInterface *iface);
G_DEFINE_TYPE_WITH_CODE (SelectionDocument, selection_document, PPS_TYPE_DOCUMENT, G_IMPLEMENT_INTERFACE (PPS_TYPE_SELECTION, selection_iface_init))

static gboolean
load (PpsDocument *document, const char *uri, GError **error)
{
	return TRUE;
}
static int
n_pages (PpsDocument *document)
{
	return 2;
}
static void
page_size (PpsDocument *document, PpsPage *page, double *width, double *height)
{
	*width = *height = 200;
}
static cairo_surface_t *
render (PpsDocument *document, PpsRenderContext *context)
{
	return cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 200, 200);
}
static void
selection_document_init (SelectionDocument *document)
{
}
static void
selection_document_class_init (SelectionDocumentClass *klass)
{
	klass->load = load;
	klass->get_n_pages = n_pages;
	klass->get_page_size = page_size;
	klass->render = render;
}
static char *
selected_text (PpsSelection *selection, PpsPage *page, PpsSelectionStyle style, PpsRectangle *rect)
{
	SelectionDocument *document = (SelectionDocument *) selection;
	/* Reproduce a callback replacing the live selection during a text request. */
	if (document->calls++ == 0)
		pps_view_select_all (document->view);
	return g_strdup_printf ("page%d", page->index);
}
static cairo_region_t *
selection_region (PpsSelection *selection, PpsRenderContext *context, PpsSelectionStyle style, PpsRectangle *rect)
{
	return cairo_region_create ();
}
static void
selection_iface_init (PpsSelectionInterface *iface)
{
	iface->get_selected_text = selected_text;
	iface->get_selection_region = selection_region;
}

static void
selection_changes_during_text_request (void)
{
	g_autoptr (PpsDocument) document = g_object_new (selection_document_get_type (), NULL);
	g_autoptr (GError) error = NULL;
	g_assert_true (pps_document_load (document, "test://selection", &error));
	g_assert_no_error (error);
	pps_document_setup_cache (document);
	g_autoptr (PpsAnnotationModel) annotations = pps_annotation_model_new ();
	g_autoptr (PpsDocumentModel) model = g_object_new (PPS_TYPE_DOCUMENT_MODEL, "annotation-model", annotations, NULL);
	PpsView *view = PPS_VIEW (pps_view_new ());
	g_object_ref_sink (view);
	pps_view_set_model (view, model);
	pps_document_model_set_document (model, document);
	((SelectionDocument *) document)->view = view;
	pps_view_select_all (view);
	g_autofree char *text = pps_view_get_selected_text (view);
	g_assert_cmpstr (text, ==, "page0page1");
	g_assert_cmpuint (((SelectionDocument *) document)->calls, ==, 2);
	g_object_unref (view);
}
int
main (int argc, char **argv)
{
	gtk_init ();
	pps_init ();
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/view/selection-reentrant-text-request", selection_changes_during_text_request);
	int result = g_test_run ();
	pps_shutdown ();
	return result;
}

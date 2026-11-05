// SPDX-License-Identifier: GPL-2.0-or-later

#include <math.h>
#include <unistd.h>

#include "pps-document-annotation-private.h"
#include "pps-document-info-private.h"
#include "pps-document-misc.h"
#include "pps-document-proxy-private.h"
#include "pps-document-security.h"

typedef struct {
	PpsDocumentSession *session;
	GVariant *pages;
	PpsDocumentInfo *info;
	PpsDocumentObjects *objects;
	GVariant *metadata;
	char *backend_name;
	char *backend_version;
	char *password;
	int n_pages;
	gboolean has_security;
	guint state;
	gulong closed_handler;
} PpsDocumentProxyPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (PpsDocumentProxy, pps_document_proxy, PPS_TYPE_DOCUMENT)

static guint failed_signal;

static PpsDocumentProxyPrivate *
get_private (gpointer document)
{
	return pps_document_proxy_get_instance_private (PPS_DOCUMENT_PROXY (document));
}

PpsDocumentSession *
_pps_document_proxy_get_session (PpsDocument *document)
{
	return get_private (document)->session;
}

GVariant *
_pps_document_proxy_get_metadata (PpsDocument *document)
{
	return get_private (document)->metadata;
}

PpsDocumentObjects *
_pps_document_proxy_get_objects (PpsDocument *document)
{
	return get_private (document)->objects;
}

guint
_pps_document_proxy_get_state (PpsDocument *document)
{
	return get_private (document)->state;
}

void
_pps_document_proxy_set_state (PpsDocument *document, guint state)
{
	get_private (document)->state = state;
	pps_document_set_modified (document, (state & PPS_DOCUMENT_STATE_MODIFIED) != 0);
}

static gboolean
read_info (PpsDocument *document, int n_pages, GError **error)
{
	PpsDocumentProxyPrivate *priv = get_private (document);
	g_autoptr (GVariant) metadata = NULL;
	g_autoptr (GVariant) pages = NULL;
	g_autoptr (PpsDocumentInfo) info = NULL;
	g_autofree char *backend_name = NULL;
	g_autofree char *backend_version = NULL;
	gboolean has_security;
	guint state;

	if (!pps_dbus_document_call_get_info_sync (
		priv->session->proxy, G_DBUS_CALL_FLAGS_NONE, -1,
		&metadata, &pages, &backend_name, &backend_version,
		&has_security, NULL, error))
		return FALSE;
	info = _pps_document_info_from_variant (metadata, error);
	if (!info)
		return FALSE;
	if (!g_variant_lookup (metadata, "document-state", "u", &state) ||
	    (state & ~(PPS_DOCUMENT_STATE_MODIFIED | PPS_DOCUMENT_STATE_FORMS_MODIFIED | PPS_DOCUMENT_STATE_ANNOTATIONS_MODIFIED))) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid document modification state");
		return FALSE;
	}
	if (n_pages < 0 || g_variant_n_children (pages) != (gsize) n_pages) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		                     "Invalid document page count");
		return FALSE;
	}
	for (int i = 0; i < n_pages; i++) {
		double width, height;
		const char *label;
		g_variant_get_child (pages, i, "(dd&s)", &width, &height, &label);
		if (!isfinite (width) || !isfinite (height) || width < 0 || height < 0) {
			g_set_error_literal (error, PPS_DOCUMENT_ERROR, PPS_DOCUMENT_ERROR_INVALID,
			                     "Document contains an invalid page size");
			return FALSE;
		}
	}
	priv->pages = g_steal_pointer (&pages);
	priv->info = g_steal_pointer (&info);
	priv->backend_name = g_steal_pointer (&backend_name);
	priv->backend_version = g_steal_pointer (&backend_version);
	priv->has_security = has_security;
	priv->metadata = g_steal_pointer (&metadata);
	priv->n_pages = n_pages;
	_pps_document_proxy_set_state (document, state);
	return TRUE;
}

static gboolean
load (PpsDocument *document, const char *uri, GError **error)
{
	PpsDocumentProxyPrivate *priv = get_private (document);
	int n_pages;

	return pps_dbus_document_call_load_sync (
		   priv->session->proxy, uri, priv->password ? priv->password : "",
		   G_DBUS_CALL_FLAGS_NONE, -1, &n_pages, NULL, error) &&
	       read_info (document, n_pages, error);
}

static gboolean
load_fd (PpsDocument *document, int fd, GError **error)
{
	PpsDocumentProxyPrivate *priv = get_private (document);
	g_autoptr (GUnixFDList) descriptors = g_unix_fd_list_new ();
	int handle = g_unix_fd_list_append (descriptors, fd, error);
	int n_pages;

	close (fd);
	if (handle == -1)
		return FALSE;
	return pps_dbus_document_call_load_fd_sync (
		   priv->session->proxy, g_variant_new_handle (handle),
		   priv->password ? priv->password : "", G_DBUS_CALL_FLAGS_NONE, -1,
		   descriptors, &n_pages, NULL, NULL, error) &&
	       read_info (document, n_pages, error);
}

static gboolean
save (PpsDocument *document, const char *uri, GError **error)
{
	PpsDocumentProxyPrivate *priv = get_private (document);
	guint state;
	if (!pps_dbus_document_call_save_sync (priv->session->proxy, uri,
	                                       G_DBUS_CALL_FLAGS_NONE, -1, &state, NULL, error))
		return FALSE;
	_pps_document_proxy_set_state (document, state);
	return TRUE;
}

static int
get_n_pages (PpsDocument *document)
{
	return get_private (document)->n_pages;
}

static void
get_page_size (PpsDocument *document, PpsPage *page, double *width, double *height)
{
	const char *label;
	double w, h;
	g_variant_get_child (get_private (document)->pages, page->index,
	                     "(dd&s)", &w, &h, &label);
	if (width)
		*width = w;
	if (height)
		*height = h;
}

static char *
get_page_label (PpsDocument *document, PpsPage *page)
{
	const char *label;
	double width, height;
	g_variant_get_child (get_private (document)->pages, page->index,
	                     "(dd&s)", &width, &height, &label);
	return g_strdup (label);
}

static PpsDocumentInfo *
get_info (PpsDocument *document)
{
	PpsDocumentProxyPrivate *priv = get_private (document);
	return priv->info ? pps_document_info_copy (priv->info) : pps_document_info_new ();
}

static gboolean
get_backend_info (PpsDocument *document, PpsDocumentBackendInfo *info)
{
	PpsDocumentProxyPrivate *priv = get_private (document);
	if (!priv->backend_name || !priv->backend_name[0])
		return FALSE;
	info->name = priv->backend_name;
	info->version = priv->backend_version;
	return TRUE;
}

static cairo_surface_t *
render_surface (PpsDocument *document, PpsRenderContext *context, gboolean thumbnail)
{
	g_autoptr (GError) error = NULL;
	return _pps_document_session_render (get_private (document)->session,
	                                     context, thumbnail, NULL, &error);
}

static cairo_surface_t *
render (PpsDocument *document, PpsRenderContext *context)
{
	return render_surface (document, context, FALSE);
}

static cairo_surface_t *
get_thumbnail_surface (PpsDocument *document, PpsRenderContext *context)
{
	return render_surface (document, context, TRUE);
}

static GdkPixbuf *
get_thumbnail (PpsDocument *document, PpsRenderContext *context)
{
	cairo_surface_t *surface = get_thumbnail_surface (document, context);
	GdkPixbuf *pixbuf;
	if (!surface)
		return NULL;
	pixbuf = pps_document_misc_pixbuf_from_surface (surface);
	cairo_surface_destroy (surface);
	return pixbuf;
}

static gboolean
has_document_security (PpsDocumentSecurity *document)
{
	return get_private (document)->has_security;
}

static void
set_password (PpsDocumentSecurity *document, const char *password)
{
	PpsDocumentProxyPrivate *priv = get_private (document);
	g_free (priv->password);
	priv->password = g_strdup (password);
}

static void
security_iface_init (PpsDocumentSecurityInterface *iface)
{
	iface->has_document_security = has_document_security;
	iface->set_password = set_password;
}

static void
connection_closed (GDBusConnection *connection, gboolean remote_peer_vanished, GError *error, PpsDocumentProxy *document)
{
	g_autoptr (GError) closed = NULL;
	if (!remote_peer_vanished)
		return;
	if (!error) {
		closed = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CLOSED,
		                              "The document worker exited");
		error = closed;
	}
	g_signal_emit (document, failed_signal, 0, error);
}

static void
dispose (GObject *object)
{
	PpsDocumentProxyPrivate *priv = get_private (object);
	if (priv->closed_handler) {
		g_signal_handler_disconnect (priv->session->connection, priv->closed_handler);
		priv->closed_handler = 0;
	}
	g_clear_pointer (&priv->session, _pps_document_session_free);
	G_OBJECT_CLASS (pps_document_proxy_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
	PpsDocumentProxyPrivate *priv = get_private (object);
	g_clear_pointer (&priv->pages, g_variant_unref);
	g_clear_pointer (&priv->info, pps_document_info_free);
	g_clear_pointer (&priv->objects, _pps_document_objects_free);
	g_clear_pointer (&priv->metadata, g_variant_unref);
	g_free (priv->backend_name);
	g_free (priv->backend_version);
	g_free (priv->password);
	G_OBJECT_CLASS (pps_document_proxy_parent_class)->finalize (object);
}

static void
pps_document_proxy_class_init (PpsDocumentProxyClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	PpsDocumentClass *document_class = PPS_DOCUMENT_CLASS (klass);
	object_class->dispose = dispose;
	object_class->finalize = finalize;
	document_class->load = load;
	document_class->load_fd = load_fd;
	document_class->save = save;
	document_class->get_n_pages = get_n_pages;
	document_class->get_page_size = get_page_size;
	document_class->get_page_label = get_page_label;
	document_class->get_info = get_info;
	document_class->get_backend_info = get_backend_info;
	document_class->render = render;
	document_class->get_thumbnail = get_thumbnail;
	document_class->get_thumbnail_surface = get_thumbnail_surface;
	failed_signal = g_signal_new ("failed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
	                              0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_ERROR);
}

static void
pps_document_proxy_init (PpsDocumentProxy *document)
{
	get_private (document)->objects = _pps_document_objects_new ();
}

GType
_pps_document_proxy_type_for_interfaces (const char *const *interfaces, guint annotation_methods)
{
	static GMutex mutex;
	GType type;
	guint mask = annotation_methods << 16;
	const struct {
		GType type;
		GInterfaceInitFunc init;
	} capabilities[] = {
		{ PPS_TYPE_DOCUMENT_SECURITY, (GInterfaceInitFunc) security_iface_init },
		{ PPS_TYPE_DOCUMENT_TEXT, (GInterfaceInitFunc) _pps_document_proxy_text_iface_init },
		{ PPS_TYPE_DOCUMENT_FIND, (GInterfaceInitFunc) _pps_document_proxy_find_iface_init },
		{ PPS_TYPE_SELECTION, (GInterfaceInitFunc) _pps_document_proxy_selection_iface_init },
		{ PPS_TYPE_DOCUMENT_LINKS, (GInterfaceInitFunc) _pps_document_proxy_links_iface_init },
		{ PPS_TYPE_DOCUMENT_LAYERS, (GInterfaceInitFunc) _pps_document_proxy_layers_iface_init },
		{ PPS_TYPE_DOCUMENT_FORMS, (GInterfaceInitFunc) _pps_document_proxy_forms_iface_init },
		{ PPS_TYPE_DOCUMENT_ATTACHMENTS, (GInterfaceInitFunc) _pps_document_proxy_attachments_iface_init },
		{ PPS_TYPE_DOCUMENT_ANNOTATIONS, (GInterfaceInitFunc) _pps_document_proxy_annotations_iface_init },
		{ PPS_TYPE_DOCUMENT_FONTS, (GInterfaceInitFunc) _pps_document_proxy_fonts_iface_init },
	};
	for (guint i = 0; i < G_N_ELEMENTS (capabilities); i++) {
		if (g_strv_contains (interfaces, g_type_name (capabilities[i].type)))
			mask |= 1u << i;
	}
	g_autofree char *name = g_strdup_printf ("PpsDocumentProxy%u", mask);

	g_mutex_lock (&mutex);
	type = g_type_from_name (name);
	if (!type) {
		type = g_type_register_static_simple (PPS_TYPE_DOCUMENT_PROXY, name,
		                                      sizeof (PpsDocumentProxyClass), NULL,
		                                      sizeof (PpsDocumentProxy), NULL, 0);
		for (guint i = 0; i < G_N_ELEMENTS (capabilities); i++) {
			if (mask & (1u << i)) {
				const GInterfaceInfo info = { capabilities[i].init, NULL, GUINT_TO_POINTER (annotation_methods) };
				g_type_add_interface_static (type, capabilities[i].type, &info);
			}
		}
	}
	g_mutex_unlock (&mutex);
	return type;
}

PpsDocument *
_pps_document_proxy_new (const char *mime_type, GCancellable *cancellable, GError **error)
{
	g_autoptr (PpsDocumentSession) session = _pps_document_session_new (cancellable, error);
	g_auto (GStrv) interfaces = NULL;
	PpsDocument *document;
	PpsDocumentProxyPrivate *priv;
	guint annotation_methods;

	if (!session || !pps_dbus_document_call_create_sync (
			    session->proxy, mime_type, G_DBUS_CALL_FLAGS_NONE, -1,
			    &interfaces, &annotation_methods, cancellable, error))
		return NULL;
	if (annotation_methods & ~(PPS_ANNOTATION_METHOD_ADD | PPS_ANNOTATION_METHOD_REMOVE | PPS_ANNOTATION_METHOD_OVER_MARKUP)) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid annotation capabilities");
		return NULL;
	}
	document = g_object_new (_pps_document_proxy_type_for_interfaces ((const char *const *) interfaces, annotation_methods), NULL);
	priv = get_private (document);
	priv->session = g_steal_pointer (&session);
	priv->closed_handler = g_signal_connect_object (priv->session->connection, "closed",
	                                                G_CALLBACK (connection_closed), document, 0);
	return document;
}

// SPDX-License-Identifier: GPL-2.0-or-later
/* this file is part of papers, a gnome document viewer
 *
 *  Copyright (C) 2015 Igalia S.L.
 */

#include <config.h>

#include "pps-media-private.h"
#include <glib/gstdio.h>

struct _PpsMediaPrivate {
	guint page;
	gchar *uri;
	gboolean show_controls;
};

typedef struct _PpsMediaPrivate PpsMediaPrivate;

#define GET_PRIVATE(o) pps_media_get_instance_private (o)

G_DEFINE_TYPE_WITH_PRIVATE (PpsMedia, pps_media, G_TYPE_OBJECT)

static void
pps_media_finalize (GObject *object)
{
	PpsMedia *media = PPS_MEDIA (object);
	PpsMediaPrivate *priv = GET_PRIVATE (media);

	g_clear_pointer (&priv->uri, g_free);

	G_OBJECT_CLASS (pps_media_parent_class)->finalize (object);
}

static void
pps_media_class_init (PpsMediaClass *klass)
{
	GObjectClass *g_object_class = G_OBJECT_CLASS (klass);

	g_object_class->finalize = pps_media_finalize;
}

static void
pps_media_init (PpsMedia *media)
{
}

PpsMedia *
pps_media_new_for_uri (PpsPage *page,
                       const gchar *uri)
{
	PpsMedia *media;
	PpsMediaPrivate *priv;

	g_return_val_if_fail (PPS_IS_PAGE (page), NULL);
	g_return_val_if_fail (uri != NULL, NULL);

	media = PPS_MEDIA (g_object_new (PPS_TYPE_MEDIA, NULL));
	priv = GET_PRIVATE (media);
	priv->page = page->index;
	priv->uri = g_strdup (uri);

	return media;
}

const gchar *
pps_media_get_uri (PpsMedia *media)
{
	g_return_val_if_fail (PPS_IS_MEDIA (media), NULL);
	PpsMediaPrivate *priv = GET_PRIVATE (media);

	return priv->uri;
}

guint
pps_media_get_page_index (PpsMedia *media)
{
	g_return_val_if_fail (PPS_IS_MEDIA (media), 0);
	PpsMediaPrivate *priv = GET_PRIVATE (media);

	return priv->page;
}

gboolean
pps_media_get_show_controls (PpsMedia *media)
{
	g_return_val_if_fail (PPS_IS_MEDIA (media), FALSE);
	PpsMediaPrivate *priv = GET_PRIVATE (media);

	return priv->show_controls;
}

void
pps_media_set_show_controls (PpsMedia *media,
                             gboolean show_controls)
{
	g_return_if_fail (PPS_IS_MEDIA (media));
	PpsMediaPrivate *priv = GET_PRIVATE (media);

	priv->show_controls = show_controls;
}

static void
delete_temp_file (gpointer data)
{
	g_unlink (data);
	g_free (data);
}

void
_pps_media_set_temp_file (PpsMedia *media, GFile *file)
{
	char *path = g_file_get_path (file);
	g_return_if_fail (path != NULL);
	GQuark key = g_quark_from_static_string ("pps-media-temp-file-path");
	if (!g_object_replace_qdata (G_OBJECT (file), key, NULL, path, delete_temp_file, NULL))
		g_free (path);
	g_object_set_data_full (G_OBJECT (media), "pps-media-temp-file", g_object_ref (file), g_object_unref);
}

GFile *
_pps_media_get_temp_file (PpsMedia *media)
{
	return g_object_get_data (G_OBJECT (media), "pps-media-temp-file");
}

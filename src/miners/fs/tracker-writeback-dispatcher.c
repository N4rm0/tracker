/*
 * Copyright (C) 2010, Nokia <ivan.frade@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <libtracker-common/tracker-dbus.h>
#include <libtracker-sparql/tracker-sparql.h>
#include <libtracker-miner/tracker-miner-dbus.h>

#include "tracker-writeback-dispatcher.h"


typedef struct {
	TrackerMinerFiles *files_miner;
	GDBusConnection *d_connection;
	TrackerSparqlConnection *connection;
	gulong signal_id;
} TrackerWritebackDispatcherPrivate;

enum {
	PROP_0,
	PROP_FILES_MINER
};

#define TRACKER_WRITEBACK_DISPATCHER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), TRACKER_TYPE_WRITEBACK_DISPATCHER, TrackerWritebackDispatcherPrivate))

static void     writeback_dispatcher_set_property    (GObject              *object,
                                                      guint                 param_id,
                                                      const GValue         *value,
                                                      GParamSpec           *pspec);
static void     writeback_dispatcher_get_property    (GObject              *object,
                                                      guint                 param_id,
                                                      GValue               *value,
                                                      GParamSpec           *pspec);
static void     writeback_dispatcher_finalize        (GObject              *object);
static gboolean writeback_dispatcher_initable_init   (GInitable            *initable,
                                                      GCancellable         *cancellable,
                                                      GError              **error);
static gboolean writeback_dispatcher_writeback_file  (TrackerMinerFS       *fs,
                                                      GFile                *file,
                                                      GPtrArray            *results,
                                                      gpointer              user_data);


static void
writeback_dispatcher_initable_iface_init (GInitableIface *iface)
{
	iface->init = writeback_dispatcher_initable_init;
}

G_DEFINE_TYPE_WITH_CODE (TrackerWritebackDispatcher, tracker_writeback_dispatcher, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                writeback_dispatcher_initable_iface_init));

static void
tracker_writeback_dispatcher_class_init (TrackerWritebackDispatcherClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = writeback_dispatcher_finalize;
	object_class->set_property = writeback_dispatcher_set_property;
	object_class->get_property = writeback_dispatcher_get_property;

	g_object_class_install_property (object_class,
	                                 PROP_FILES_MINER,
	                                 g_param_spec_object ("files_miner",
	                                                      "files_miner",
	                                                      "The FS Miner",
	                                                      TRACKER_TYPE_MINER_FILES,
	                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_type_class_add_private (klass, sizeof (TrackerWritebackDispatcherPrivate));
}

static void
writeback_dispatcher_set_property (GObject      *object,
                                   guint         param_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
	TrackerWritebackDispatcherPrivate *priv;

	priv = TRACKER_WRITEBACK_DISPATCHER_GET_PRIVATE (object);

	switch (param_id) {
	case PROP_FILES_MINER:
		priv->files_miner = g_value_dup_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	}
}


static void
writeback_dispatcher_get_property (GObject    *object,
                                   guint       param_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
	TrackerWritebackDispatcherPrivate *priv;

	priv = TRACKER_WRITEBACK_DISPATCHER_GET_PRIVATE (object);

	switch (param_id) {
	case PROP_FILES_MINER:
		g_value_set_object (value, priv->files_miner);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, param_id, pspec);
		break;
	}
}

static void
writeback_dispatcher_finalize (GObject *object)
{
	TrackerWritebackDispatcherPrivate *priv = TRACKER_WRITEBACK_DISPATCHER_GET_PRIVATE (object);

	if (priv->signal_id != 0 && g_signal_handler_is_connected (object, priv->signal_id)) {
		g_signal_handler_disconnect (object, priv->signal_id);
	}

	if (priv->connection) {
		g_object_unref (priv->connection);
	}

	if (priv->d_connection) {
		g_object_unref (priv->d_connection);
	}

	if (priv->files_miner) {
		g_object_unref (priv->files_miner);
	}
}


static void
tracker_writeback_dispatcher_init (TrackerWritebackDispatcher *object)
{
}

static gboolean
writeback_dispatcher_initable_init (GInitable    *initable,
                                    GCancellable *cancellable,
                                    GError       **error)
{
	TrackerWritebackDispatcherPrivate *priv;
	GError *internal_error = NULL;

	priv = TRACKER_WRITEBACK_DISPATCHER_GET_PRIVATE (initable);

	priv->connection = tracker_sparql_connection_get (NULL, &internal_error);

	if (internal_error) {
		g_propagate_error (error, internal_error);
		return FALSE;
	}

	priv->d_connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &internal_error);

	if (internal_error) {
		g_propagate_error (error, internal_error);
		return FALSE;
	}

	priv->signal_id = g_signal_connect_object (priv->files_miner,
	                                           "writeback-file",
	                                           G_CALLBACK (writeback_dispatcher_writeback_file),
	                                           initable,
	                                           G_CONNECT_AFTER);

	return TRUE;
}

TrackerWritebackDispatcher *
tracker_writeback_dispatcher_new (TrackerMinerFiles  *miner_files,
                                  GError            **error)
{
	GObject *miner;
	GError *internal_error = NULL;

	miner =  g_initable_new (TRACKER_TYPE_WRITEBACK_DISPATCHER,
	                         NULL,
	                         &internal_error,
	                         "files-miner", miner_files,
	                         NULL);

	if (internal_error) {
		g_propagate_error (error, internal_error);
		return NULL;
	}

	return (TrackerWritebackDispatcher *) miner;
}

static gboolean
writeback_dispatcher_writeback_file (TrackerMinerFS *fs,
                                     GFile          *file,
                                     GPtrArray      *results,
                                     gpointer        user_data)
{
	TrackerWritebackDispatcher *self = user_data;
	TrackerWritebackDispatcherPrivate *priv;
	// GError *internal_error = NULL;
	gchar *uri;
	guint i;

	priv = TRACKER_WRITEBACK_DISPATCHER_GET_PRIVATE (self);

	uri = g_file_get_uri (file);
	g_print ("Writeback: %s with:\n", uri);

	for (i = 0; i< results->len; i++) {
		GStrv row = g_ptr_array_index (results, i);
		guint y;

		g_print ("\t");
		for (y = 0; row[y] != NULL; y++) {
			if (y != 0) {
				g_print (",");
			}
			g_print ("%d=%s", y, row[y]);
		}
		g_print ("\n");
	}

	g_free (uri);

	return TRUE;
}

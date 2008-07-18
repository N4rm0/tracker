/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2006, Mr Jamie McCracken (jamiemcc@gnome.org)
 * Copyright (C) 2008, Nokia

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

/* The indexer works as a state machine, there are 3 different queues:
 *
 * * The files queue: the highest priority one, individual files are
 *   stored here, waiting for metadata extraction, etc... files are
 *   taken one by one in order to be processed, when this queue is
 *   empty, a single token from the next queue is processed.
 *
 * * The directories queue: directories are stored here, waiting for
 *   being inspected. When a directory is inspected, contained files
 *   and directories will be prepended in their respective queues.
 *   When this queue is empty, a single token from the next queue
 *   is processed.
 *
 * * The modules list: indexing modules are stored here, these modules
 *   can either prepend the files or directories to be inspected in
 *   their respective queues.
 *
 * Once all queues are empty, all elements have been inspected, and the
 * indexer will emit the ::finished signal, this behavior can be observed
 * in the process_func() function.
 *
 * NOTE: Normally all indexing petitions will be sent over DBus, being
 *       everything just pushed in the files queue.
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>

#include <glib/gstdio.h>
#include <gmodule.h>

#include <libtracker-common/tracker-config.h>
#include <libtracker-common/tracker-dbus.h>
#include <libtracker-common/tracker-file-utils.h>
#include <libtracker-common/tracker-hal.h>
#include <libtracker-common/tracker-language.h>
#include <libtracker-common/tracker-parser.h>
#include <libtracker-common/tracker-ontology.h>
#include <libtracker-common/tracker-module-config.h>
#include <libtracker-common/tracker-utils.h>

#include <libtracker-db/tracker-db-manager.h>
#include <libtracker-db/tracker-db-interface-sqlite.h>

#include "tracker-indexer.h"
#include "tracker-indexer-module.h"
#include "tracker-indexer-db.h"
#include "tracker-index.h"
#include "tracker-module.h"
#include "tracker-marshal.h"

#define TRACKER_INDEXER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), TRACKER_TYPE_INDEXER, TrackerIndexerPrivate))

/* Flush every 'x' seconds */
#define FLUSH_FREQUENCY             5

/* Transaction every 'x' items */
#define TRANSACTION_MAX             200

/* Throttle defaults */
#define THROTTLE_DEFAULT            0
#define THROTTLE_DEFAULT_ON_BATTERY 5

typedef struct PathInfo PathInfo;
typedef struct MetadataForeachData MetadataForeachData;

struct TrackerIndexerPrivate {
	GQueue *dir_queue;
	GQueue *file_queue;
	GQueue *modules_queue;

	GList *module_names;
	gchar *current_module_name;
	GHashTable *indexer_modules;

	gchar *db_dir;

	TrackerIndex *index;

	TrackerDBInterface *metadata;
	TrackerDBInterface *contents;
	TrackerDBInterface *common;
	TrackerDBInterface *cache;

	TrackerConfig *config;
	TrackerLanguage *language;

#ifdef HAVE_HAL 
	TrackerHal *hal;
#endif /* HAVE_HAL */

	GTimer *timer;

	guint idle_id;
	guint flush_id;

	guint items_processed;
	guint items_indexed;

	gboolean in_transaction;
};

struct PathInfo {
	GModule *module;
	TrackerFile *file;
};

struct MetadataForeachData {
	TrackerIndex *index;
	TrackerDBInterface *db;

	TrackerLanguage *language;
	TrackerConfig *config;
	TrackerService *service;
	guint32 id;
};

enum {
	PROP_0,
	PROP_RUNNING,
};

enum {
	STATUS,
	STARTED,
	FINISHED,
	MODULE_STARTED,
	MODULE_FINISHED,
	PAUSED,
	CONTINUED,
	LAST_SIGNAL
};

static gboolean process_func (gpointer data);

static guint signals[LAST_SIGNAL] = { 0, };

G_DEFINE_TYPE (TrackerIndexer, tracker_indexer, G_TYPE_OBJECT)

static PathInfo *
path_info_new (GModule *module,
	       const gchar *module_name,
	       const gchar *path)
{
	PathInfo *info;

	info = g_slice_new (PathInfo);
	info->module = module;
	info->file = tracker_indexer_module_file_new (module, module_name, path);

	return info;
}

static void
path_info_free (PathInfo *info)
{
	tracker_indexer_module_file_free (info->module, info->file);
	g_slice_free (PathInfo, info);
}

static void 
start_transaction (TrackerIndexer *indexer)
{
	g_debug ("Transaction start");

	indexer->private->in_transaction = TRUE;

	tracker_db_interface_start_transaction (indexer->private->cache);
	tracker_db_interface_start_transaction (indexer->private->contents);
	tracker_db_interface_start_transaction (indexer->private->metadata);
	tracker_db_interface_start_transaction (indexer->private->common);
}

static void 
stop_transaction (TrackerIndexer *indexer)
{
	tracker_db_interface_end_transaction (indexer->private->common);
	tracker_db_interface_end_transaction (indexer->private->metadata);
	tracker_db_interface_end_transaction (indexer->private->contents);
	tracker_db_interface_end_transaction (indexer->private->cache);

	indexer->private->items_indexed += indexer->private->items_processed;
	indexer->private->items_processed = 0;
	indexer->private->in_transaction = FALSE;

	g_debug ("Transaction commit");
}

static void
signal_status (TrackerIndexer *indexer,
	       const gchar    *why)
{
	gdouble seconds_elapsed;
	guint   items_remaining;

	items_remaining = g_queue_get_length (indexer->private->file_queue);
	seconds_elapsed = g_timer_elapsed (indexer->private->timer, NULL);

	if (indexer->private->items_indexed > 0 && 
	    items_remaining > 0) {
		gchar *str1;
		gchar *str2;

		str1 = tracker_seconds_estimate_to_string (seconds_elapsed, 
							   TRUE,
							   indexer->private->items_indexed, 
							   items_remaining);
		str2 = tracker_seconds_to_string (seconds_elapsed, TRUE);
		
		g_message ("Indexed %d/%d, module:'%s', %s left, %s elapsed (%s)", 
			   indexer->private->items_indexed,
			   indexer->private->items_indexed + items_remaining,
			   indexer->private->current_module_name,
			   str1,
			   str2,
			   why);

		g_free (str2);
		g_free (str1);
	}
	
	g_signal_emit (indexer, signals[STATUS], 0, 
		       seconds_elapsed,
		       indexer->private->current_module_name,
		       indexer->private->items_indexed,
		       items_remaining);
}

static gboolean
schedule_flush_cb (gpointer data)
{
	TrackerIndexer *indexer;

	indexer = TRACKER_INDEXER (data);

	indexer->private->flush_id = 0;

	/* If we have transactions, we don't need to flush, we
	 * just need to end the transactions on each
	 * interface. This performs a commit for us. 
	 */
	if (indexer->private->in_transaction) {
		stop_transaction (indexer);
	} else {
		indexer->private->items_indexed += tracker_index_flush (indexer->private->index);
	}

	signal_status (indexer, "timed flush");

	return FALSE;
}

static void
schedule_flush (TrackerIndexer *indexer,
		gboolean        immediately)
{
	if (immediately) {
		/* No need to wait for flush timeout */
		if (indexer->private->flush_id) {
			g_source_remove (indexer->private->flush_id);
			indexer->private->flush_id = 0;
		}

		/* If we have transactions, we don't need to flush, we
		 * just need to end the transactions on each
		 * interface. This performs a commit for us. 
		 */
		if (indexer->private->in_transaction) {
			stop_transaction (indexer);
		} else {
			indexer->private->items_indexed += tracker_index_flush (indexer->private->index);
		}

		signal_status (indexer, "immediate flush");

		return;
	}

	/* Don't schedule more than one at the same time */
	if (indexer->private->flush_id != 0) {
		return;
	}

	indexer->private->flush_id = g_timeout_add_seconds (FLUSH_FREQUENCY, 
							    schedule_flush_cb, 
							    indexer);
}

static void
set_up_throttle (TrackerIndexer *indexer)
{
#ifdef HAVE_HAL
	gint throttle;

	/* If on a laptop battery and the throttling is default (i.e.
	 * 0), then set the throttle to be higher so we don't kill
	 * the laptop battery.
	 */
	throttle = tracker_config_get_throttle (indexer->private->config);

	if (tracker_hal_get_battery_in_use (indexer->private->hal)) {
		g_message ("We are running on battery");
		
		if (throttle == THROTTLE_DEFAULT) {
			tracker_config_set_throttle (indexer->private->config, 
						     THROTTLE_DEFAULT_ON_BATTERY);
			g_message ("Setting throttle from %d to %d", 
				   throttle, 
				   THROTTLE_DEFAULT_ON_BATTERY);
		} else {
			g_message ("Not setting throttle, it is currently set to %d",
				   throttle);
		}
	} else {
		g_message ("We are not running on battery");

		if (throttle == THROTTLE_DEFAULT_ON_BATTERY) {
			tracker_config_set_throttle (indexer->private->config, 
						     THROTTLE_DEFAULT);
			g_message ("Setting throttle from %d to %d", 
				   throttle, 
				   THROTTLE_DEFAULT);
		} else {
			g_message ("Not setting throttle, it is currently set to %d", 
				   throttle);
		}
	}
#else  /* HAVE_HAL */
	g_message ("HAL is not available to know if we are using a battery or not.");
	g_message ("Not setting the throttle");
#endif /* HAVE_HAL */
}

static void
notify_battery_in_use_cb (GObject *gobject,
			  GParamSpec *arg1,
			  gpointer user_data) 
{
	set_up_throttle (TRACKER_INDEXER (user_data));
}

static void
tracker_indexer_finalize (GObject *object)
{
	TrackerIndexerPrivate *priv;

	priv = TRACKER_INDEXER_GET_PRIVATE (object);

	/* Important! Make sure we flush if we are scheduled to do so,
	 * and do that first.
	 */
	if (priv->flush_id) {
		g_source_remove (priv->flush_id);
		schedule_flush (TRACKER_INDEXER (object), TRUE);
	}

	if (priv->idle_id) {
		g_source_remove (priv->idle_id);
	}

	if (priv->timer) {
		g_timer_destroy (priv->timer);
	}

#ifdef HAVE_HAL
	g_signal_handlers_disconnect_by_func (priv->hal, 
					      notify_battery_in_use_cb,
					      TRACKER_INDEXER (object));
	
	g_object_unref (priv->hal);
#endif /* HAVE_HAL */

	g_object_unref (priv->language);
	g_object_unref (priv->config);

	/* Do we destroy interfaces? I can't remember */

	tracker_index_free (priv->index);

	g_free (priv->db_dir);

	g_hash_table_unref (priv->indexer_modules);
	g_free (priv->current_module_name);
	g_list_free (priv->module_names);

	g_queue_foreach (priv->modules_queue, (GFunc) g_free, NULL);
	g_queue_free (priv->modules_queue);

	g_queue_foreach (priv->dir_queue, (GFunc) path_info_free, NULL);
	g_queue_free (priv->dir_queue);

	g_queue_foreach (priv->file_queue, (GFunc) path_info_free, NULL);
	g_queue_free (priv->file_queue);

	/* The queue doesn't own the module names */
	g_queue_free (priv->modules_queue);

	G_OBJECT_CLASS (tracker_indexer_parent_class)->finalize (object);
}

static void
tracker_indexer_get_property (GObject    *object,
			      guint       prop_id,
			      GValue     *value,
			      GParamSpec *pspec)
{
	TrackerIndexerPrivate *priv;

	priv = TRACKER_INDEXER_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_RUNNING:
		g_value_set_boolean (value, (priv->idle_id != 0));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	}
}

static void
tracker_indexer_class_init (TrackerIndexerClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);

	object_class->finalize = tracker_indexer_finalize;
	object_class->get_property = tracker_indexer_get_property;

	signals[STATUS] =
		g_signal_new ("status",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerIndexerClass, status),
			      NULL, NULL,
			      tracker_marshal_VOID__DOUBLE_STRING_UINT_UINT,
			      G_TYPE_NONE,
			      4,
			      G_TYPE_DOUBLE,
			      G_TYPE_STRING,
			      G_TYPE_UINT,
			      G_TYPE_UINT);
	signals[STARTED] = 
		g_signal_new ("started",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerIndexerClass, started),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 
			      0);
	signals[PAUSED] = 
		g_signal_new ("paused",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerIndexerClass, paused),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 
			      0);
	signals[CONTINUED] = 
		g_signal_new ("continued",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerIndexerClass, continued),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 
			      0);
	signals[FINISHED] = 
		g_signal_new ("finished",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerIndexerClass, finished),
			      NULL, NULL,
			      tracker_marshal_VOID__DOUBLE_UINT,
			      G_TYPE_NONE, 
			      2,
			      G_TYPE_DOUBLE,
			      G_TYPE_UINT);
	signals[MODULE_STARTED] =
		g_signal_new ("module-started",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerIndexerClass, module_started),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_STRING);
	signals[MODULE_FINISHED] =
		g_signal_new ("module-finished",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerIndexerClass, module_finished),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE,
			      1,
			      G_TYPE_STRING);

	g_object_class_install_property (object_class,
					 PROP_RUNNING,
					 g_param_spec_boolean ("running",
							       "Running",
							       "Whether the indexer is running",
							       TRUE,
							       G_PARAM_READABLE));

	g_type_class_add_private (object_class, sizeof (TrackerIndexerPrivate));
}

static void
close_module (GModule *module)
{
	tracker_indexer_module_shutdown (module);
	g_module_close (module);
}

static void
check_started (TrackerIndexer *indexer)
{
	if (indexer->private->idle_id) {
		return;
	}

	indexer->private->idle_id = g_idle_add (process_func, indexer);
	
	g_timer_destroy (indexer->private->timer);
	indexer->private->timer = g_timer_new ();

	g_signal_emit (indexer, signals[STARTED], 0);
}

static void
check_stopped (TrackerIndexer *indexer)
{
	gchar   *str;
	gdouble  seconds_elapsed;

	if (indexer->private->idle_id == 0) {
		return;
	}

	/* Flush remaining items */
	schedule_flush (indexer, TRUE);

	/* No more modules to query, we're done */
	g_timer_stop (indexer->private->timer);
	seconds_elapsed = g_timer_elapsed (indexer->private->timer, NULL);

	/* Clean up source ID */
	indexer->private->idle_id = 0;
	
	/* Print out how long it took us */
	str = tracker_seconds_to_string (seconds_elapsed, FALSE);

	g_message ("Indexer finished in %s, %d items indexed in total",
		   str,
		   indexer->private->items_indexed);
	g_free (str);

	/* Finally signal done */
	g_signal_emit (indexer, signals[FINISHED], 0, 
		       seconds_elapsed,
		       indexer->private->items_indexed);
}

static void
tracker_indexer_init (TrackerIndexer *indexer)
{
	TrackerIndexerPrivate *priv;
	gchar *index_file;
	GList *l;

	priv = indexer->private = TRACKER_INDEXER_GET_PRIVATE (indexer);

	priv->items_processed = 0;
	priv->in_transaction = FALSE;
	priv->dir_queue = g_queue_new ();
	priv->file_queue = g_queue_new ();
	priv->modules_queue = g_queue_new ();
	priv->config = tracker_config_new ();

#ifdef HAVE_HAL 
	priv->hal = tracker_hal_new ();

	g_signal_connect (priv->hal, "notify::battery-in-use",
			  G_CALLBACK (notify_battery_in_use_cb),
			  indexer);

	set_up_throttle (indexer);
#endif /* HAVE_HAL */

	priv->language = tracker_language_new (priv->config);

	priv->db_dir = g_build_filename (g_get_user_cache_dir (),
					 "tracker", 
					 NULL);

	priv->module_names = tracker_module_config_get_modules ();

	priv->indexer_modules = g_hash_table_new_full (g_str_hash,
						       g_str_equal,
						       NULL,
						       (GDestroyNotify) close_module);

	for (l = priv->module_names; l; l = l->next) {
		GModule *module;
		
		if (!tracker_module_config_get_enabled (l->data)) {
			continue;
		}

		module = tracker_indexer_module_load (l->data);

		if (module) {
			tracker_indexer_module_init (module);

			g_hash_table_insert (priv->indexer_modules,
					     l->data, module);
		}
	}

	/* Set up indexer */
	index_file = g_build_filename (priv->db_dir, "file-index.db", NULL);
	priv->index = tracker_index_new (index_file,
					 tracker_config_get_max_bucket_count (priv->config));
	g_free (index_file);

	/* Set up databases */
	priv->cache = tracker_db_manager_get_db_interface (TRACKER_DB_CACHE);
	priv->common = tracker_db_manager_get_db_interface (TRACKER_DB_COMMON);
	priv->metadata = tracker_db_manager_get_db_interface (TRACKER_DB_FILE_METADATA);
	priv->contents = tracker_db_manager_get_db_interface (TRACKER_DB_FILE_CONTENTS);

	/* Set up timer to know how long the process will take and took */
	priv->timer = g_timer_new ();

	/* Set up idle handler to process files/directories */
	check_started (indexer);
}

static void
add_file (TrackerIndexer *indexer,
	  PathInfo *info)
{
	g_queue_push_tail (indexer->private->file_queue, info);

	/* Make sure we are still running */
	check_started (indexer);
}

static void
add_directory (TrackerIndexer *indexer,
	       PathInfo *info)
{
	g_queue_push_tail (indexer->private->dir_queue, info);

	/* Make sure we are still running */
	check_started (indexer);
}

static void
indexer_throttle (TrackerConfig *config,
		  gint multiplier)
{
        gint throttle;

        throttle = tracker_config_get_throttle (config);

        if (throttle < 1) {
                return;
        }

        throttle *= multiplier;

        if (throttle > 0) {
                g_usleep (throttle);
        }
}

static void
index_metadata_foreach (gpointer key,
			gpointer value,
			gpointer user_data)
{
	TrackerField *field;
	MetadataForeachData *data;
	gchar *parsed_value;
	gchar **arr;
	gint throttle;
	gint i;

	if (!value) {
		return;
	}

	data = (MetadataForeachData *) user_data;

	/* Throttle indexer, value 9 is from older code, why 9? */
	throttle = tracker_config_get_throttle (data->config);
	if (throttle > 9) {
		indexer_throttle (data->config, throttle * 100);
	}

	/* Parse */
	field = tracker_ontology_get_field_def ((gchar *) key);

	parsed_value = tracker_parser_text_to_string ((gchar *) value,
						      data->language,
						      tracker_config_get_max_word_length (data->config),
						      tracker_config_get_min_word_length (data->config),
						      tracker_field_get_filtered (field),
						      tracker_field_get_filtered (field),
						      tracker_field_get_delimited (field));
	arr = g_strsplit (parsed_value, " ", -1);

	for (i = 0; arr[i]; i++) {
		tracker_index_add_word (data->index,
					arr[i],
					data->id,
					tracker_service_get_id (data->service),
					tracker_field_get_weight (field));
	}

	tracker_db_set_metadata (data->db, data->id, field, (gchar *) value, parsed_value);

	g_free (parsed_value);
	g_strfreev (arr);
}

static void
index_metadata (TrackerIndexer *indexer,
		guint32 id,
		TrackerService *service,
		GHashTable *metadata)
{
	MetadataForeachData data;

	data.index = indexer->private->index;
	data.db = indexer->private->metadata;
	data.language = indexer->private->language;
	data.config = indexer->private->config;
	data.service = service;
	data.id = id;

	g_hash_table_foreach (metadata, index_metadata_foreach, &data);

	schedule_flush (indexer, FALSE);
}

static gboolean
process_file (TrackerIndexer *indexer,
	      PathInfo *info)
{
	GHashTable *metadata;

	g_debug ("Processing file:'%s'", info->file->path);

	/* Set the current module */
	g_free (indexer->private->current_module_name);
	indexer->private->current_module_name = g_strdup (info->file->module_name);
	
	/* Sleep to throttle back indexing */
	indexer_throttle (indexer->private->config, 100);

	/* Process file */
	metadata = tracker_indexer_module_file_get_metadata (info->module, info->file);

	if (metadata) {
		TrackerService *service_def;
		gchar *service_type, *mimetype;
		gboolean created;
		guint32 id;

		mimetype = tracker_file_get_mime_type (info->file->path);
		service_type = tracker_ontology_get_service_type_for_mime (mimetype);
		service_def = tracker_ontology_get_service_type_by_name (service_type);
		id = tracker_db_get_new_service_id (indexer->private->common);

		created = tracker_db_create_service (indexer->private->metadata, 
						     id, 
						     service_def, 
						     info->file->path, 
						     metadata);

		g_free (service_type);
		g_free (mimetype);
		
		if (created) {
			gchar *text;
			guint32 eid;
			gboolean inc_events = FALSE;

			eid = tracker_db_get_new_event_id (indexer->private->common);

			created = tracker_db_create_event (indexer->private->cache, 
							   eid, 
							   id,
							   "Create");
			if (created) {
				inc_events = TRUE;
			}

			tracker_db_increment_stats (indexer->private->common, service_def);

			index_metadata (indexer, id, service_def, metadata);

			text = tracker_indexer_module_file_get_text (info->module, info->file);

			if (text) {
				tracker_db_set_text (indexer->private->contents, id, text);
				g_free (text);
			}

			if (inc_events) {
				tracker_db_inc_event_id (indexer->private->common, eid);
			}

			tracker_db_inc_service_id (indexer->private->common, id);
		}

		g_hash_table_destroy (metadata);
	}

	indexer->private->items_processed++;

	return !tracker_indexer_module_file_iter_contents (info->module, info->file);
}

static void
process_directory (TrackerIndexer *indexer,
		   PathInfo *info,
		   gboolean recurse)
{
	const gchar *name;
	GDir *dir;

	g_debug ("Processing directory:'%s'", info->file->path);

	dir = g_dir_open (info->file->path, 0, NULL);

	if (!dir) {
		return;
	}

	while ((name = g_dir_read_name (dir)) != NULL) {
		PathInfo *new_info;
		gchar *path;

		path = g_build_filename (info->file->path, name, NULL);

		new_info = path_info_new (info->module, info->file->module_name, path);
		add_file (indexer, new_info);

		if (recurse && g_file_test (path, G_FILE_TEST_IS_DIR)) {
			new_info = path_info_new (info->module, info->file->module_name, path);
			add_directory (indexer, new_info);
		}

		g_free (path);
	}

	g_dir_close (dir);
}

static void
process_module_emit_signals (TrackerIndexer *indexer,
			     const gchar *next_module_name)
{
	/* Signal the last module as finished */
	g_signal_emit (indexer, signals[MODULE_FINISHED], 0, 
		       indexer->private->current_module_name);

	/* Set current module */
	g_free (indexer->private->current_module_name);
	indexer->private->current_module_name = g_strdup (next_module_name);

	/* Signal the next module as started */
	if (next_module_name) {
		g_signal_emit (indexer, signals[MODULE_STARTED], 0, 
			       next_module_name);
	}
}

static void
process_module (TrackerIndexer *indexer,
		const gchar *module_name)
{
	GModule *module;
	GList *dirs, *d;

	module = g_hash_table_lookup (indexer->private->indexer_modules, module_name);

	/* Signal module start/stop */
	process_module_emit_signals (indexer, module_name);

	if (!module) {
		/* No need to signal stopped here, we will get that
		 * signal the next time this function is called.
		 */
		g_message ("No module for:'%s'", module_name);
		return;
	}

	g_message ("Starting module:'%s'", module_name);
	
	dirs = tracker_module_config_get_monitor_recurse_directories (module_name);
	g_return_if_fail (dirs != NULL);

	for (d = dirs; d; d = d->next) {
		PathInfo *info;

		info = path_info_new (module, module_name, d->data);
		add_directory (indexer, info);
	}

	g_list_free (dirs);
}

static gboolean
process_func (gpointer data)
{
	TrackerIndexer *indexer;
	PathInfo *path;

	indexer = TRACKER_INDEXER (data);

	if (!indexer->private->in_transaction) {
		start_transaction (indexer);
	}

	if ((path = g_queue_peek_head (indexer->private->file_queue)) != NULL) {
		/* Process file */
		if (process_file (indexer, path)) {
			path = g_queue_pop_head (indexer->private->file_queue);
			path_info_free (path);
		}
	} else if ((path = g_queue_pop_head (indexer->private->dir_queue)) != NULL) {
		/* Process directory contents */
		process_directory (indexer, path, TRUE);
		path_info_free (path);
	} else {
		gchar *module_name;

		/* Dirs/files queues are empty, process the next module */
		module_name = g_queue_pop_head (indexer->private->modules_queue);

		if (!module_name) {
			/* Signal the last module as finished */
			process_module_emit_signals (indexer, NULL);
			
			/* Signal stopped and clean up */
			check_stopped (indexer);

			return FALSE;
		}

		process_module (indexer, module_name);
		g_free (module_name);
	}

	if (indexer->private->items_processed > TRANSACTION_MAX) {
		schedule_flush (indexer, TRUE);
	}

	return TRUE;
}

TrackerIndexer *
tracker_indexer_new (void)
{
	return g_object_new (TRACKER_TYPE_INDEXER, NULL);
}

gboolean
tracker_indexer_get_is_running (TrackerIndexer *indexer) 
{
	g_return_val_if_fail (TRACKER_IS_INDEXER (indexer), FALSE);

	return indexer->private->idle_id != 0;
}


/**
 * This one is not being used yet, but might be used in near future. Therefore
 * it would be useful if Garnacho could review this for consistency and 
 * correctness. Ps. it got written by that pvanhoof dude, just ping him if you
 * have questions.
 **/

void
tracker_indexer_set_paused (TrackerIndexer         *indexer,
			    gboolean                paused,
			    DBusGMethodInvocation  *context,
			    GError                **error)
{
	g_return_if_fail (TRACKER_IS_INDEXER (indexer));

	if (tracker_indexer_get_is_running (indexer)) {
		if (paused) {
			if (indexer->private->in_transaction) {
				stop_transaction (indexer);
			}
			if (indexer->private->idle_id) {
				g_source_remove (indexer->private->idle_id);
				indexer->private->idle_id = 0;
			}
			g_signal_emit (indexer, signals[PAUSED], 0);
		}
	} else {
		if (!paused) {
			if (!indexer->private->idle_id) {
				indexer->private->idle_id = g_idle_add (process_func, indexer);
				g_signal_emit (indexer, signals[CONTINUED], 0);
			}
		}
	}
	dbus_g_method_return (context);
}


void
tracker_indexer_process_all (TrackerIndexer *indexer)
{
	GList *l;

	for (l = indexer->private->module_names; l; l = l->next) {
		g_queue_push_tail (indexer->private->modules_queue, g_strdup (l->data));
	}
}

void
tracker_indexer_files_check (TrackerIndexer *indexer,
			     const gchar *module_name,
			     GStrv files,
			     DBusGMethodInvocation *context,
			     GError **error)
{
	GModule *module;
	guint request_id;
	gint i;
	GError *actual_error = NULL;

	tracker_dbus_async_return_if_fail (TRACKER_IS_INDEXER (indexer), FALSE);
	tracker_dbus_async_return_if_fail (files != NULL, FALSE);

	request_id = tracker_dbus_get_next_request_id ();

	tracker_dbus_request_new (request_id,
                                  "DBus request to check %d files",
				  g_strv_length (files));

	module = g_hash_table_lookup (indexer->private->indexer_modules, module_name);

	if (module) {
		/* Add files to the queue */
		for (i = 0; files[i]; i++) {
			PathInfo *info;

			info = path_info_new (module, module_name, files[i]);
			add_file (indexer, info);
		}
	} else {
		tracker_dbus_request_failed (request_id,
					     &actual_error,
					     "The module '%s' is not loaded",
					     module_name);
	}

	if (!actual_error) {
		dbus_g_method_return (context);
		tracker_dbus_request_success (request_id);
	} else {
		dbus_g_method_return_error (context, actual_error);
		g_error_free (actual_error);
	}
}

void
tracker_indexer_files_update (TrackerIndexer *indexer,
			      const gchar *module_name,
			      GStrv files,
			      DBusGMethodInvocation *context,
			      GError **error)
{
	GModule *module;
	guint request_id;
	gint i;
	GError *actual_error = NULL;

	tracker_dbus_async_return_if_fail (TRACKER_IS_INDEXER (indexer), FALSE);
	tracker_dbus_async_return_if_fail (files != NULL, FALSE);

	request_id = tracker_dbus_get_next_request_id ();

	tracker_dbus_request_new (request_id,
                                  "DBus request to update %d files",
				  g_strv_length (files));

	module = g_hash_table_lookup (indexer->private->indexer_modules, module_name);

	if (module) {
		/* Add files to the queue */
		for (i = 0; files[i]; i++) {
			PathInfo *info;

			info = path_info_new (module, module_name, files[i]);
			add_file (indexer, info);
		}
	} else {
		tracker_dbus_request_failed (request_id,
					     &actual_error,
					     "The module '%s' is not loaded",
					     module_name);
	}

	if (!actual_error) {
		dbus_g_method_return (context);
		tracker_dbus_request_success (request_id);
	} else {
		dbus_g_method_return_error (context, actual_error);
		g_error_free (actual_error);
	}
}

void
tracker_indexer_files_delete (TrackerIndexer *indexer,
			      const gchar *module_name,
			      GStrv files,
			      DBusGMethodInvocation *context,
			      GError **error)
{
	GModule *module;
	guint request_id;
	gint i;
	GError *actual_error = NULL;

	tracker_dbus_async_return_if_fail (TRACKER_IS_INDEXER (indexer), FALSE);
	tracker_dbus_async_return_if_fail (files != NULL, FALSE);

	request_id = tracker_dbus_get_next_request_id ();

	tracker_dbus_request_new (request_id,
                                  "DBus request to delete %d files",
				  g_strv_length (files));

	module = g_hash_table_lookup (indexer->private->indexer_modules, module_name);

	if (module) {
		/* Add files to the queue */
		for (i = 0; files[i]; i++) {
			PathInfo *info;

			info = path_info_new (module, module_name, files[i]);
			add_file (indexer, info);
		}

	} else {
		tracker_dbus_request_failed (request_id,
					     &actual_error,
					     "The module '%s' is not loaded",
					     module_name);
	}

	if (!actual_error) {
		dbus_g_method_return (context);
		tracker_dbus_request_success (request_id);
	} else {
		dbus_g_method_return_error (context, actual_error);
		g_error_free (actual_error);
	}
}


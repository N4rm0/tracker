/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2006, Mr Jamie McCracken (jamiemcc@gnome.org)
 * Copyright (C) 2007, Jason Kivlighn (jkivlighn@gmail.com)
 * Copyright (C) 2007, Creative Commons (http://creativecommons.org) 
 * Copyright (C) 2008, Nokia
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

#ifndef __TRACKERD_DB_H__
#define __TRACKERD_DB_H__

#include <glib.h>

#include <libtracker-common/tracker-field.h>
#include <libtracker-common/tracker-field-data.h>
#include <libtracker-common/tracker-ontology.h>

#include <libtracker-db/tracker-db-interface.h>
#include <libtracker-db/tracker-db-file-info.h>

#include "tracker-indexer.h"
#include "tracker-utils.h"

G_BEGIN_DECLS

void                tracker_db_init                            (void);
void                tracker_db_shutdown                        (void);
void                tracker_db_refresh_all                     (TrackerDBInterface  *iface);


/* Operations for TrackerDBInterface */
void                tracker_db_close                           (TrackerDBInterface  *iface);
TrackerDBResultSet *tracker_db_exec_proc                       (TrackerDBInterface  *iface,
								const gchar         *procedure,
								...);
gboolean            tracker_db_exec_no_reply                   (TrackerDBInterface  *iface,
								const gchar         *query,
								...);
TrackerDBResultSet *tracker_db_exec                            (TrackerDBInterface  *iface,
								const char          *query,
								...);
gchar *             tracker_db_get_option_string               (const gchar         *option);
void                tracker_db_set_option_string               (const gchar         *option,
								const gchar         *value);
gint                tracker_db_get_option_int                  (const gchar         *option);
void                tracker_db_set_option_int                  (const gchar         *option,
								gint                 value);

/* High level transactions things */
gboolean            tracker_db_regulate_transactions           (TrackerDBInterface  *iface,
								gint                 interval);

/* Metadata API */
gchar *             tracker_db_metadata_get_related_names      (TrackerDBInterface  *iface,
								const gchar         *name);
const gchar *       tracker_db_metadata_get_table              (TrackerFieldType     type);
TrackerDBResultSet *tracker_db_metadata_get                    (TrackerDBInterface  *iface,
								const gchar         *id,
								const gchar         *key);
gchar *             tracker_db_metadata_get_delimited          (TrackerDBInterface  *iface,
								const gchar         *id,
								const gchar         *key);
gchar *             tracker_db_metadata_set                    (TrackerDBInterface  *iface,
								const gchar         *service,
								const gchar         *id,
								const gchar         *key,
								gchar              **values,
								gboolean             do_backup);
void                tracker_db_metadata_set_single             (TrackerDBInterface  *iface,
								const gchar         *service,
								const gchar         *id,
								const gchar         *key,
								const gchar         *value,
								gboolean             do_backup);
void                tracker_db_metadata_insert_embedded        (TrackerDBInterface  *iface,
								const gchar         *service,
								const gchar         *id,
								const gchar         *key,
								gchar              **values,
								GHashTable          *table);
void                tracker_db_metadata_insert_single_embedded (TrackerDBInterface  *iface,
								const gchar         *service,
								const gchar         *id,
								const gchar         *key,
								const gchar         *value,
								GHashTable          *table);
void                tracker_db_metadata_delete_value           (TrackerDBInterface  *iface,
								const gchar         *service,
								const gchar         *id,
								const gchar         *key,
								const gchar         *value);
void                tracker_db_metadata_delete                 (TrackerDBInterface  *iface,
								const gchar         *service,
								const gchar         *id,
								const gchar         *key,
								gboolean             update_indexes);
TrackerDBResultSet *tracker_db_metadata_get_types              (TrackerDBInterface  *iface,
								const gchar         *class,
								gboolean             writeable);

/* Search API */
TrackerDBResultSet *tracker_db_search_text                     (TrackerDBInterface  *iface,
								const gchar         *service,
								const gchar         *search_string,
								gint                 offset,
								gint                 limit,
								gboolean             save_results,
								gboolean             detailed);

/* Service API */
guint32             tracker_db_service_create                  (TrackerDBInterface  *iface,
								const gchar         *service,
								TrackerDBFileInfo   *info);
gchar *             tracker_db_service_get_by_entity           (TrackerDBInterface  *iface,
								const gchar         *id);

/* Files API */
gchar **            tracker_db_files_get                       (TrackerDBInterface  *iface,
								const gchar         *folder_uri);
TrackerDBResultSet *tracker_db_files_get_by_service            (TrackerDBInterface  *iface,
								const gchar         *service,
								gint                 offset,
								gint                 limit);
TrackerDBResultSet *tracker_db_files_get_by_mime               (TrackerDBInterface  *iface,
								gchar              **mimes,
								gint                 n,
								gint                 offset,
								gint                 limit,
								gboolean             vfs);
guint32             tracker_db_file_get_id                     (TrackerDBInterface  *iface,
								const gchar         *uri);
gchar *             tracker_db_file_get_id_as_string           (TrackerDBInterface  *iface,
								const gchar         *service,
								const gchar         *uri);
TrackerDBFileInfo * tracker_db_file_get_info                   (TrackerDBInterface  *iface,
								TrackerDBFileInfo   *info);
gboolean            tracker_db_file_is_up_to_date              (TrackerDBInterface  *iface,
								const gchar         *uri,
								guint32             *id);
void                tracker_db_file_delete                     (TrackerDBInterface  *iface,
								guint32              file_id);
void                tracker_db_file_move                       (TrackerDBInterface  *iface,
								const gchar         *moved_from_uri,
								const gchar         *moved_to_uri);
void                tracker_db_directory_delete                (TrackerDBInterface  *iface,
								guint32              file_id,
								const gchar         *uri);
void                tracker_db_directory_move                  (TrackerDBInterface  *iface,
								const gchar         *moved_from_uri,
								const gchar         *moved_to_uri);
void                tracker_db_uri_insert_pending              (const gchar         *id,
								const gchar         *action,
								const gchar         *counter,
								const gchar         *uri,
								const gchar         *mime,
								gboolean             is_dir,
								gboolean             is_new,
								gint                 service_type_id);
void                tracker_db_uri_update_pending              (const gchar         *counter,
								const gchar         *action,
								const gchar         *uri);
TrackerDBResultSet *tracker_db_uri_get_subfolders              (TrackerDBInterface  *iface,
								const gchar         *uri);
TrackerDBResultSet *tracker_db_uri_sub_watches_get             (const gchar         *dir);
TrackerDBResultSet *tracker_db_uri_sub_watches_delete          (const gchar         *dir);

/* Keywords API */
TrackerDBResultSet *tracker_db_keywords_get_list               (TrackerDBInterface  *iface,
								const gchar         *service);

/* Miscellaneous API */
GHashTable *        tracker_db_get_file_contents_words         (TrackerDBInterface  *iface,
								guint32              id,
								GHashTable          *old_table);
GHashTable *        tracker_db_get_indexable_content_words     (TrackerDBInterface  *iface,
								guint32              id,
								GHashTable          *table,
								gboolean             embedded_only);
gchar *             tracker_db_get_field_name                  (const gchar         *service,
								const gchar         *meta_name);
TrackerFieldData *  tracker_db_get_metadata_field              (TrackerDBInterface  *iface,
								const gchar         *service,
								const gchar         *field_name,
								gint                 field_count,
								gboolean             is_select,
								gboolean             is_condition);

/* Live Search API */
void                tracker_db_live_search_start               (TrackerDBInterface  *iface,
								const gchar         *from_query,
								const gchar         *join_query,
								const gchar         *where_query,
								const gchar         *search_id);
void                tracker_db_live_search_stop                (TrackerDBInterface  *iface,
								const gchar         *search_id);
TrackerDBResultSet *tracker_db_live_search_get_all_ids         (TrackerDBInterface  *iface,
								const gchar         *search_id);
TrackerDBResultSet *tracker_db_live_search_get_new_ids         (TrackerDBInterface  *iface,
								const gchar         *search_id,
								const gchar         *from_query,
								const gchar         *query_joins,
								const gchar         *where_query);
TrackerDBResultSet *tracker_db_live_search_get_deleted_ids     (TrackerDBInterface  *iface,
								const gchar         *search_id);
TrackerDBResultSet *tracker_db_live_search_get_hit_data        (TrackerDBInterface  *iface,
								const gchar         *search_id);
TrackerDBResultSet *tracker_db_live_search_get_hit_count       (TrackerDBInterface  *iface,
								const gchar         *search_id);

/* XESAM API */
void                tracker_db_xesam_delete_handled_events     (TrackerDBInterface  *iface);
TrackerDBResultSet *tracker_db_xesam_get_metadata_names        (TrackerDBInterface  *iface,
								const char          *name);
TrackerDBResultSet *tracker_db_xesam_get_service_names         (TrackerDBInterface  *iface,
								const char          *name);

G_END_DECLS

#endif /* __TRACKERD_DB_H__ */

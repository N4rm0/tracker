/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
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

#include <stdio.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include <libtracker-common/tracker-file-utils.h>

#include "tracker-db-index-manager.h"

#define MIN_BUCKET_DEFAULT 10
#define MAX_BUCKET_DEFAULT 20

#define TRACKER_DB_INDEX_FILENAME         "index.db"
#define TRACKER_DB_INDEX_UPDATE_FILENAME  "index-update.db"

#define MAX_INDEX_FILE_SIZE 2000000000

typedef struct {
        TrackerDBIndexType  type;
	TrackerDBIndex     *index;
        const gchar        *file;
        const gchar        *name;
        gchar              *abs_filename;
} TrackerDBIndexDefinition;

static gboolean                 initialized;
static gchar                   *data_dir;

static TrackerDBIndexDefinition indexes[] = {
        { TRACKER_DB_INDEX_TYPE_INDEX, 
	  NULL,
          TRACKER_DB_INDEX_FILENAME,
          "index",
          NULL },
        { TRACKER_DB_INDEX_TYPE_INDEX_UPDATE, 
	  NULL,
          TRACKER_DB_INDEX_UPDATE_FILENAME,
          "index-update",
          NULL }
};

static gboolean
has_tmp_merge_files (TrackerDBIndexType type)
{
	GFile           *file;
	GFileEnumerator *enumerator;
	GFileInfo       *info;
	GError          *error = NULL;
	gchar           *prefix;
	const gchar     *data_dir;
	gchar           *dirname;
	gboolean         found;

	if (type != TRACKER_DB_INDEX_TYPE_INDEX) {
		return FALSE;
	}

	prefix = g_strconcat (indexes[TRACKER_DB_INDEX_TYPE_INDEX].name, ".tmp", NULL);

	dirname = g_path_get_dirname (indexes[type].abs_filename);
	file = g_file_new_for_path (dirname);

	enumerator = g_file_enumerate_children (file,
						G_FILE_ATTRIBUTE_STANDARD_NAME ","
						G_FILE_ATTRIBUTE_STANDARD_TYPE,
						G_PRIORITY_DEFAULT,
						NULL, 
						&error);

	if (error) {
		g_warning ("Could not check for temporary index files in "
			   "directory:'%s', %s",
			   dirname,
			   error->message);

		g_error_free (error);
		g_object_unref (file);
		g_free (dirname);
		g_free (prefix);

		return FALSE;
	}
	
	found = FALSE;

	for (info = g_file_enumerator_next_file (enumerator, NULL, &error);
	     info && !error && !found;
	     info = g_file_enumerator_next_file (enumerator, NULL, &error)) {
		/* Check each file has or hasn't got the prefix */
		if (g_str_has_prefix (g_file_info_get_name (info), prefix)) {
			found = TRUE;
		}

		g_object_unref (info);
	}

	if (error) {
		g_warning ("Could not get file information for temporary "
			   "index files in directory:'%s', %s",
			   data_dir,
			   error->message);
		g_error_free (error);
	}
		
	g_object_unref (enumerator);
	g_object_unref (file);
	g_free (dirname);
	g_free (prefix);

	return found;
}

gboolean
tracker_db_index_manager_init (TrackerDBIndexManagerFlags  flags,
			       gint                        min_bucket, 
			       gint                        max_bucket)
{
	gchar    *final_index_filename;
	gchar    *name;
	gboolean  need_reindex = FALSE;
	gboolean  force_reindex;
	gboolean  readonly;
	guint     i;

	g_return_val_if_fail (min_bucket >= 0, FALSE);
	g_return_val_if_fail (max_bucket >= min_bucket, FALSE);

        if (initialized) {
                return TRUE;
        }

	g_message ("Setting index database locations");

	data_dir = g_build_filename (g_get_user_cache_dir (), 
				     "tracker", 
				     NULL);

	g_message ("Checking index directories exist");

	g_mkdir_with_parents (data_dir, 00755);

	g_message ("Checking index files exist");

	for (i = 0; i < G_N_ELEMENTS (indexes); i++) {
		indexes[i].abs_filename = g_build_filename (data_dir, indexes[i].file, NULL);

		if (need_reindex) {
			continue;
		}

		if (!g_file_test (indexes[i].abs_filename, G_FILE_TEST_EXISTS)) { 
			g_message ("Could not find index file:'%s'", indexes[i].abs_filename);
		}
	}
	
	g_message ("Merging old temporary indexes");
	
	i = TRACKER_DB_INDEX_TYPE_INDEX;
	name = g_strconcat (indexes[i].name, "-final", NULL);
	final_index_filename = g_build_filename (data_dir, name, NULL);
	g_free (name);
	
	if (g_file_test (final_index_filename, G_FILE_TEST_EXISTS) && 
	    !has_tmp_merge_files (i)) {
		g_message ("  Overwriting '%s' with '%s'", 
			   indexes[i].abs_filename, 
			   final_index_filename);	
		
		g_rename (final_index_filename, indexes[i].abs_filename);
	}
	
	g_free (final_index_filename);
	
	/* Now we have cleaned up merge files, see if we are supposed
	 * to be reindexing.
	 */ 

	force_reindex = (flags & TRACKER_DB_INDEX_MANAGER_FORCE_REINDEX) != 0;

	if (force_reindex || need_reindex) {
		g_message ("Cleaning up index files for reindex");

		for (i = 0; i < G_N_ELEMENTS (indexes); i++) {
			g_unlink (indexes[i].abs_filename);
		}
	}

	g_message ("Creating index files, this may take a few moments...");
	
	readonly = (flags & TRACKER_DB_INDEX_MANAGER_READONLY) != 0;

	for (i = 0; i < G_N_ELEMENTS (indexes); i++) {
		indexes[i].index = tracker_db_index_new (indexes[i].abs_filename,
							 min_bucket, 
							 max_bucket, 
							 readonly);
	}

        initialized = TRUE;

	return TRUE;
}

void
tracker_db_index_manager_shutdown (void)
{
	guint i;

        if (!initialized) {
                return;
        }
        
	for (i = 0; i < G_N_ELEMENTS (indexes); i++) {
		g_object_unref (indexes[i].index);
		indexes[i].index = NULL;

		g_free (indexes[i].abs_filename);
		indexes[i].abs_filename = NULL;
	}

	g_free (data_dir);

        initialized = FALSE;
}

TrackerDBIndex * 
tracker_db_index_manager_get_index (TrackerDBIndexType type)
{
	return indexes[type].index;
}

const gchar *
tracker_db_index_manager_get_filename (TrackerDBIndexType type)
{
        return indexes[type].abs_filename;
}

gboolean
tracker_db_index_manager_are_indexes_too_big (void)
{
        gboolean too_big;
	guint    i;

	g_return_val_if_fail (initialized == TRUE, FALSE);

	for (i = 0, too_big = FALSE; i < G_N_ELEMENTS (indexes) && !too_big; i++) {
		too_big = tracker_file_get_size (indexes[i].abs_filename) > MAX_INDEX_FILE_SIZE;
	}
        
        if (too_big) {
		g_critical ("One or more index files are too big, indexing disabled");
		return TRUE;	
	}

	return FALSE;
}

/*
 * Copyright (C) 2008, Nokia <ivan.frade@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#include "config.h"

#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "tracker-albumart.h"

#include <libtracker-common/tracker-albumart.h>
#include <libtracker-sparql/tracker-sparql.h>

static gboolean had_any = FALSE;

static void
on_query_finished (GObject      *source_object,
                   GAsyncResult *res,
                   gpointer      user_data)
{
	GError *error = NULL;
	TrackerSparqlCursor *cursor = NULL;
	GDir *dir = NULL;
	GHashTable *table = NULL;
	const gchar *name;
	gchar *dirname = NULL;
	GList *to_remove = NULL;

	cursor = tracker_sparql_connection_query_finish (TRACKER_SPARQL_CONNECTION (source_object),
	                                                 res,
	                                                 &error);

	if (error) {
		goto on_error;
	}

	dirname = g_build_filename (g_get_user_cache_dir (),
	                            "media-art",
	                            NULL);

	if (!g_file_test (dirname, G_FILE_TEST_EXISTS)) {
		goto on_error;
	}

	dir = g_dir_open (dirname, 0, &error);

	if (error) {
		goto on_error;
	}

	table = g_hash_table_new_full (g_str_hash,
	                               g_str_equal,
	                               (GDestroyNotify) g_free,
	                               (GDestroyNotify) g_free);

	while (tracker_sparql_cursor_next (cursor, NULL, NULL)) {
		const gchar *album = tracker_sparql_cursor_get_string (cursor, 0, NULL);
		gchar *album_stripped, *target = NULL;

		if (album) {
			album_stripped = tracker_albumart_strip_invalid_entities (album);
			tracker_albumart_get_path (NULL,
			                           album_stripped,
			                           "album", NULL,
			                           &target, NULL);
			g_hash_table_replace (table, target, album_stripped);
		}
	}

	/* Perhaps we should have an internal list of albumart files that we made,
	 * instead of going over all the albumart (which could also have been made
	 * by other softwares) */

	for (name = g_dir_read_name (dir); name != NULL; name = g_dir_read_name (dir)) {
		gpointer value;

		value = g_hash_table_lookup (table, name);

		if (!value) {
			g_message ("Removing media-art file %s: no album exists that has "
			           "more than one song for this media-art cache", name);
			to_remove = g_list_prepend (to_remove, (gpointer) name);
		}
	}

	g_list_foreach (to_remove, (GFunc) g_unlink, NULL);
	g_list_free (to_remove);

on_error:

	g_free (dirname);

	if (table) {
		g_hash_table_unref (table);
	}

	if (cursor) {
		g_object_unref (cursor);
	}

	if (dir) {
		g_dir_close (dir);
	}

	if (error) {
		g_critical ("Error running cleanup of media-art: %s",
		            error->message ? error->message : "No error given");
		g_error_free (error);
	}
}
/**
 * tracker_albumart_remove_add:
 * @connection: SPARQL connection of this miner
 * @uri: URI of the file
 * @mime_type: mime-type of the file
 *
 * Adds a new request to tell the albumart subsystem that @uri was removed.
 * Stored requests can be processed with tracker_thumbnailer_process().
 *
 * Returns: #TRUE if successfully stored to be reported, #FALSE otherwise.
 *
 * Since: 0.8
 */
gboolean
tracker_albumart_remove_add (const gchar *uri,
                             const gchar *mime_type)
{
	/* mime_type can be NULL */

	g_return_val_if_fail (uri != NULL, FALSE);

	if (!mime_type || (g_str_has_prefix (mime_type, "video/") || g_str_has_prefix (mime_type, "audio/"))) {
		had_any = TRUE;
	}

	return TRUE;
}

/**
 * tracker_albumart_process:
 *
 * Process all stored albumart requests.
 *
 * Since: 0.10
 */
void
tracker_albumart_check_cleanup (TrackerSparqlConnection *connection)
{
	if (had_any) {
		tracker_sparql_connection_query_async (connection,
		                                       "SELECT ?title WHERE { "
		                                       "   ?mpiece nmm:musicAlbum ?album . "
		                                       "   ?album nmm:albumTitle ?title "
		                                       "}",
		                                       NULL,
		                                       on_query_finished,
		                                       NULL);
		had_any = FALSE;
	}
}

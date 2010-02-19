/*
 * Copyright (C) 2006, Mr Jamie McCracken (jamiemcc@gnome.org)
 * Copyright (C) 2008, Nokia (urho.konttori@nokia.com)
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <libtracker-common/tracker-common.h>
#include <libtracker-common/tracker-date-time.h>
#include <libtracker-common/tracker-file-utils.h>
#include <libtracker-common/tracker-ontology.h>

#include <libtracker-fts/tracker-fts.h>

#include <libtracker-db/tracker-db-manager.h>
#include <libtracker-db/tracker-db-dbus.h>
#include <libtracker-db/tracker-db-journal.h>

#include "tracker-data-manager.h"
#include "tracker-data-update.h"
#include "tracker-data-query.h"
#include "tracker-sparql-query.h"

#define RDF_PREFIX TRACKER_RDF_PREFIX
#define RDFS_PREFIX TRACKER_RDFS_PREFIX
#define TRACKER_PREFIX TRACKER_TRACKER_PREFIX

typedef struct _TrackerDataUpdateBuffer TrackerDataUpdateBuffer;
typedef struct _TrackerDataUpdateBufferResource TrackerDataUpdateBufferResource;
typedef struct _TrackerDataUpdateBufferPredicate TrackerDataUpdateBufferPredicate;
typedef struct _TrackerDataUpdateBufferProperty TrackerDataUpdateBufferProperty;
typedef struct _TrackerDataUpdateBufferTable TrackerDataUpdateBufferTable;
typedef struct _TrackerDataBlankBuffer TrackerDataBlankBuffer;
typedef struct _TrackerStatementDelegate TrackerStatementDelegate;
typedef struct _TrackerCommitDelegate TrackerCommitDelegate;

struct _TrackerDataUpdateBuffer {
	/* string -> integer */
	GHashTable *resource_cache;
	/* string -> TrackerDataUpdateBufferResource */
	GHashTable *resources;
	/* valid per sqlite transaction, not just for same subject */
	gboolean fts_ever_updated;
};

struct _TrackerDataUpdateBufferResource {
	gchar *subject;
	gchar *new_subject;
	gint id;
	gboolean create;
	gboolean fts_updated;
	/* TrackerProperty -> GValueArray */
	GHashTable *predicates;
	/* string -> TrackerDataUpdateBufferTable */
	GHashTable *tables;
	/* string */
	GPtrArray *types;
};

struct _TrackerDataUpdateBufferProperty {
	gchar *name;
	GValue value;
	gchar *graph;
	gboolean fts : 1;
	gboolean date_time : 1;
};

struct _TrackerDataUpdateBufferTable {
	gboolean insert;
	gboolean delete_row;
	gboolean delete_value;
	gboolean multiple_values;
	TrackerClass *class;
	/* TrackerDataUpdateBufferProperty */
	GArray *properties;
};

/* buffer for anonymous blank nodes
 * that are not yet in the database */
struct _TrackerDataBlankBuffer {
	GHashTable *table;
	gchar *subject;
	/* string */
	GArray *predicates;
	/* string */
	GArray *objects;
	/* string */
	GArray *graphs;
};

struct _TrackerStatementDelegate {
	TrackerStatementCallback callback;
	gpointer user_data;
};

struct _TrackerCommitDelegate {
	TrackerCommitCallback callback;
	gpointer user_data;
};

static gboolean in_transaction = FALSE;
static gboolean in_journal_replay = FALSE;
static TrackerDataUpdateBuffer update_buffer;
/* current resource */
static TrackerDataUpdateBufferResource *resource_buffer;
static TrackerDataBlankBuffer blank_buffer;
static time_t resource_time = 0;

static GPtrArray *insert_callbacks = NULL;
static GPtrArray *delete_callbacks = NULL;
static GPtrArray *commit_callbacks = NULL;
static GPtrArray *rollback_callbacks = NULL;

void
tracker_data_add_commit_statement_callback (TrackerCommitCallback    callback,
                                            gpointer                 user_data)
{
	TrackerCommitDelegate *delegate = g_new0 (TrackerCommitDelegate, 1);

	if (!commit_callbacks) {
		commit_callbacks = g_ptr_array_new ();
	}

	delegate->callback = callback;
	delegate->user_data = user_data;

	g_ptr_array_add (commit_callbacks, delegate);
}

void
tracker_data_add_rollback_statement_callback (TrackerCommitCallback    callback,
                                              gpointer                 user_data)
{
	TrackerCommitDelegate *delegate = g_new0 (TrackerCommitDelegate, 1);

	if (!rollback_callbacks) {
		rollback_callbacks = g_ptr_array_new ();
	}

	delegate->callback = callback;
	delegate->user_data = user_data;

	g_ptr_array_add (rollback_callbacks, delegate);
}

void
tracker_data_add_insert_statement_callback (TrackerStatementCallback callback,
                                            gpointer                 user_data)
{
	TrackerStatementDelegate *delegate = g_new0 (TrackerStatementDelegate, 1);

	if (!insert_callbacks) {
		insert_callbacks = g_ptr_array_new ();
	}

	delegate->callback = callback;
	delegate->user_data = user_data;

	g_ptr_array_add (insert_callbacks, delegate);
}

void
tracker_data_add_delete_statement_callback (TrackerStatementCallback callback,
                                            gpointer                 user_data)
{
	TrackerStatementDelegate *delegate = g_new0 (TrackerStatementDelegate, 1);

	if (!delete_callbacks) {
		delete_callbacks = g_ptr_array_new ();
	}

	delegate->callback = callback;
	delegate->user_data = user_data;

	g_ptr_array_add (delete_callbacks, delegate);
}

GQuark tracker_data_error_quark (void) {
	return g_quark_from_static_string ("tracker_data_error-quark");
}

static gint
tracker_data_update_get_new_service_id (TrackerDBInterface *iface)
{
	TrackerDBCursor    *cursor;
	TrackerDBInterface *temp_iface;
	TrackerDBStatement *stmt;

	static gint         max = 0;

	if (G_LIKELY (max != 0)) {
		return ++max;
	}

	temp_iface = tracker_db_manager_get_db_interface ();

	stmt = tracker_db_interface_create_statement (temp_iface,
	                                              "SELECT MAX(ID) AS A FROM Resource");
	cursor = tracker_db_statement_start_cursor (stmt, NULL);
	g_object_unref (stmt);

	if (cursor) {
		tracker_db_cursor_iter_next (cursor);
		max = MAX (tracker_db_cursor_get_int (cursor, 0), max);
		g_object_unref (cursor);
	}

	return ++max;
}

static gint
tracker_data_update_get_next_modseq (void)
{
	TrackerDBCursor    *cursor;
	TrackerDBInterface *temp_iface;
	TrackerDBStatement *stmt;
	static gint         max = 0;

	if (G_LIKELY (max != 0)) {
		return ++max;
	}

	temp_iface = tracker_db_manager_get_db_interface ();

	stmt = tracker_db_interface_create_statement (temp_iface,
	                                              "SELECT MAX(\"tracker:modified\") AS A FROM \"rdfs:Resource\"");
	cursor = tracker_db_statement_start_cursor (stmt, NULL);
	g_object_unref (stmt);

	if (cursor) {
		tracker_db_cursor_iter_next (cursor);
		max = MAX (tracker_db_cursor_get_int (cursor, 0), max);
		g_object_unref (cursor);
	}

	return ++max;
}


static TrackerDataUpdateBufferTable *
cache_table_new (gboolean multiple_values)
{
	TrackerDataUpdateBufferTable *table;

	table = g_slice_new0 (TrackerDataUpdateBufferTable);
	table->multiple_values = multiple_values;
	table->properties = g_array_sized_new (FALSE, FALSE, sizeof (TrackerDataUpdateBufferProperty), 4);

	return table;
}

static void
cache_table_free (TrackerDataUpdateBufferTable *table)
{
	TrackerDataUpdateBufferProperty *property;
	gint                            i;

	for (i = 0; i < table->properties->len; i++) {
		property = &g_array_index (table->properties, TrackerDataUpdateBufferProperty, i);
		g_free (property->name);
		g_value_unset (&property->value);
		g_free (property->graph);
	}

	g_array_free (table->properties, TRUE);
	g_slice_free (TrackerDataUpdateBufferTable, table);
}

static TrackerDataUpdateBufferTable *
cache_ensure_table (const gchar            *table_name,
                    gboolean                multiple_values)
{
	TrackerDataUpdateBufferTable *table;

	table = g_hash_table_lookup (resource_buffer->tables, table_name);
	if (table == NULL) {
		table = cache_table_new (multiple_values);
		g_hash_table_insert (resource_buffer->tables, g_strdup (table_name), table);
		table->insert = multiple_values;
	}

	return table;
}

static void
cache_insert_row (TrackerClass *class)
{
	TrackerDataUpdateBufferTable    *table;

	table = cache_ensure_table (tracker_class_get_name (class), FALSE);
	table->class = class;
	table->insert = TRUE;
}

static void
cache_insert_value (const gchar            *table_name,
                    const gchar            *field_name,
                    GValue                 *value,
                    const gchar            *graph,
                    gboolean                multiple_values,
                    gboolean                fts,
                    gboolean                date_time)
{
	TrackerDataUpdateBufferTable    *table;
	TrackerDataUpdateBufferProperty  property;

	property.name = g_strdup (field_name);
	property.value = *value;
	property.graph = g_strdup (graph);
	property.fts = fts;
	property.date_time = date_time;

	table = cache_ensure_table (table_name, multiple_values);
	g_array_append_val (table->properties, property);
}

static void
cache_delete_row (TrackerClass *class)
{
	TrackerDataUpdateBufferTable    *table;

	table = cache_ensure_table (tracker_class_get_name (class), FALSE);
	table->class = class;
	table->delete_row = TRUE;
}

static void
cache_delete_value (const gchar            *table_name,
                    const gchar            *field_name,
                    GValue                 *value,
                    gboolean                multiple_values,
                    gboolean                fts,
                    gboolean                date_time)
{
	TrackerDataUpdateBufferTable    *table;
	TrackerDataUpdateBufferProperty  property;

	property.name = g_strdup (field_name);
	property.value = *value;
	property.graph = NULL;
	property.fts = fts;
	property.date_time = date_time;

	table = cache_ensure_table (table_name, multiple_values);
	table->delete_value = TRUE;
	g_array_append_val (table->properties, property);
}

static gint
query_resource_id (const gchar *uri)
{
	gint id;

	id = GPOINTER_TO_INT (g_hash_table_lookup (update_buffer.resource_cache, uri));

	if (id == 0) {
		id = tracker_data_query_resource_id (uri);

		if (id) {
			g_hash_table_insert (update_buffer.resource_cache, g_strdup (uri), GINT_TO_POINTER (id));
		}
	}

	return id;
}

static gint
ensure_resource_id (const gchar *uri,
                    gboolean    *create)
{
	TrackerDBInterface *iface, *common;
	TrackerDBStatement *stmt;

	gint id;

	id = query_resource_id (uri);

	if (create) {
		*create = (id == 0);
	}

	if (id == 0) {
		/* object resource not yet in the database */
		common = tracker_db_manager_get_db_interface ();
		iface = tracker_db_manager_get_db_interface ();

		id = tracker_data_update_get_new_service_id (common);
		stmt = tracker_db_interface_create_statement (iface, "INSERT INTO Resource (ID, Uri) VALUES (?, ?)");
		tracker_db_statement_bind_int (stmt, 0, id);
		tracker_db_statement_bind_text (stmt, 1, uri);
		tracker_db_statement_execute (stmt, NULL);
		g_object_unref (stmt);

		if (!in_journal_replay) {
			tracker_db_journal_append_resource (id, uri);
		}

		g_hash_table_insert (update_buffer.resource_cache, g_strdup (uri), GINT_TO_POINTER (id));
	}

	return id;
}

static void
statement_bind_gvalue (TrackerDBStatement *stmt,
                       gint               *idx,
                       const GValue       *value)
{
	GType type;

	type = G_VALUE_TYPE (value);
	switch (type) {
	case G_TYPE_STRING:
		tracker_db_statement_bind_text (stmt, (*idx)++, g_value_get_string (value));
		break;
	case G_TYPE_INT:
		tracker_db_statement_bind_int (stmt, (*idx)++, g_value_get_int (value));
		break;
	case G_TYPE_INT64:
		tracker_db_statement_bind_int64 (stmt, (*idx)++, g_value_get_int64 (value));
		break;
	case G_TYPE_DOUBLE:
		tracker_db_statement_bind_double (stmt, (*idx)++, g_value_get_double (value));
		break;
	default:
		if (type == TRACKER_TYPE_DATE_TIME) {
			tracker_db_statement_bind_int64 (stmt, (*idx)++, tracker_date_time_get_time (value));
			tracker_db_statement_bind_int (stmt, (*idx)++, tracker_date_time_get_local_date (value));
			tracker_db_statement_bind_int (stmt, (*idx)++, tracker_date_time_get_local_time (value));
		} else {
			g_warning ("Unknown type for binding: %s\n", G_VALUE_TYPE_NAME (value));
		}
		break;
	}
}

static void
tracker_data_resource_buffer_flush (GError **error)
{
	TrackerDBInterface             *iface;
	TrackerDBStatement             *stmt;
	TrackerDataUpdateBufferTable    *table;
	TrackerDataUpdateBufferProperty *property;
	GHashTableIter                  iter;
	const gchar                    *table_name;
	GString                        *sql, *fts;
	gint                            i, param;
	GError                         *actual_error = NULL;

	iface = tracker_db_manager_get_db_interface ();

	if (resource_buffer->new_subject != NULL) {
		/* change uri of resource */
		stmt = tracker_db_interface_create_statement (iface,
		                                              "UPDATE Resource SET Uri = ? WHERE ID = ?");
		tracker_db_statement_bind_text (stmt, 0, resource_buffer->new_subject);
		tracker_db_statement_bind_int (stmt, 1, resource_buffer->id);
		tracker_db_statement_execute (stmt, &actual_error);
		g_object_unref (stmt);

		g_free (resource_buffer->new_subject);
		resource_buffer->new_subject = NULL;

		if (actual_error) {
			g_propagate_error (error, actual_error);
			return;
		}
	}

	g_hash_table_iter_init (&iter, resource_buffer->tables);
	while (g_hash_table_iter_next (&iter, (gpointer*) &table_name, (gpointer*) &table)) {
		if (table->multiple_values) {
			for (i = 0; i < table->properties->len; i++) {
				property = &g_array_index (table->properties, TrackerDataUpdateBufferProperty, i);

				if (table->delete_value) {
					/* delete rows for multiple value properties */
					stmt = tracker_db_interface_create_statement (iface,
					                                              "DELETE FROM \"%s\" WHERE ID = ? AND \"%s\" = ?",
					                                              table_name,
					                                              property->name);
				} else if (property->date_time) {
					stmt = tracker_db_interface_create_statement (iface,
					                                              "INSERT OR IGNORE INTO \"%s\" (ID, \"%s\", \"%s:localDate\", \"%s:localTime\", \"%s:graph\") VALUES (?, ?, ?, ?, ?)",
					                                              table_name,
					                                              property->name,
					                                              property->name,
					                                              property->name,
					                                              property->name);
				} else {
					stmt = tracker_db_interface_create_statement (iface,
					                                              "INSERT OR IGNORE INTO \"%s\" (ID, \"%s\", \"%s:graph\") VALUES (?, ?, ?)",
					                                              table_name,
					                                              property->name,
					                                              property->name);
				}

				param = 0;

				tracker_db_statement_bind_int (stmt, param++, resource_buffer->id);
				statement_bind_gvalue (stmt, &param, &property->value);

				if (property->graph) {
					tracker_db_statement_bind_int (stmt, param++, ensure_resource_id (property->graph, NULL));
				} else {
					tracker_db_statement_bind_null (stmt, param++);
				}

				tracker_db_statement_execute (stmt, &actual_error);
				g_object_unref (stmt);

				if (actual_error) {
					g_propagate_error (error, actual_error);
					return;
				}
			}
		} else {
			if (table->delete_row) {
				/* remove entry from rdf:type table */
				stmt = tracker_db_interface_create_statement (iface, "DELETE FROM \"rdfs:Resource_rdf:type\" WHERE ID = ? AND \"rdf:type\" = ?");
				tracker_db_statement_bind_int (stmt, 0, resource_buffer->id);
				tracker_db_statement_bind_int (stmt, 1, ensure_resource_id (tracker_class_get_uri (table->class), NULL));
				tracker_db_statement_execute (stmt, &actual_error);
				g_object_unref (stmt);

				if (actual_error) {
					g_propagate_error (error, actual_error);
					return;
				}

				if (table->class) {
					tracker_class_set_count (table->class, tracker_class_get_count (table->class) - 1);
				}

				/* remove row from class table */
				stmt = tracker_db_interface_create_statement (iface, "DELETE FROM \"%s\" WHERE ID = ?", table_name);
				tracker_db_statement_bind_int (stmt, 0, resource_buffer->id);
				tracker_db_statement_execute (stmt, &actual_error);
				g_object_unref (stmt);

				if (actual_error) {
					g_propagate_error (error, actual_error);
					return;
				}

				continue;
			}

			if (table->insert) {
				if (strcmp (table_name, "rdfs:Resource") == 0) {
					/* ensure we have a row for the subject id */
					stmt = tracker_db_interface_create_statement (iface,
						                                      "INSERT OR IGNORE INTO \"%s\" (ID, \"tracker:added\", \"tracker:modified\", Available) VALUES (?, ?, ?, 1)",
						                                      table_name);
					tracker_db_statement_bind_int (stmt, 0, resource_buffer->id);
					g_warn_if_fail  (resource_time != 0);
					tracker_db_statement_bind_int64 (stmt, 1, (gint64) resource_time);
					tracker_db_statement_bind_int (stmt, 3, tracker_data_update_get_next_modseq ());
					tracker_db_statement_execute (stmt, &actual_error);
					g_object_unref (stmt);
				} else {
					/* ensure we have a row for the subject id */
					stmt = tracker_db_interface_create_statement (iface,
						                                      "INSERT OR IGNORE INTO \"%s\" (ID) VALUES (?)",
						                                      table_name);
					tracker_db_statement_bind_int (stmt, 0, resource_buffer->id);
					tracker_db_statement_execute (stmt, &actual_error);
					g_object_unref (stmt);
				}

				if (actual_error) {
					g_propagate_error (error, actual_error);
					return;
				}
			}

			if (table->properties->len == 0) {
				continue;
			}

			sql = g_string_new ("UPDATE ");
			g_string_append_printf (sql, "\"%s\" SET ", table_name);

			for (i = 0; i < table->properties->len; i++) {
				property = &g_array_index (table->properties, TrackerDataUpdateBufferProperty, i);
				if (i > 0) {
					g_string_append (sql, ", ");
				}
				g_string_append_printf (sql, "\"%s\" = ?", property->name);

				if (property->date_time) {
					g_string_append_printf (sql, ", \"%s:localDate\" = ?", property->name);
					g_string_append_printf (sql, ", \"%s:localTime\" = ?", property->name);
				}

				g_string_append_printf (sql, ", \"%s:graph\" = ?", property->name);
			}

			g_string_append (sql, " WHERE ID = ?");

			stmt = tracker_db_interface_create_statement (iface, "%s", sql->str);

			param = 0;
			for (i = 0; i < table->properties->len; i++) {
				property = &g_array_index (table->properties, TrackerDataUpdateBufferProperty, i);
				if (table->delete_value) {
					/* just set value to NULL for single value properties */
					tracker_db_statement_bind_null (stmt, param++);
				} else {
					statement_bind_gvalue (stmt, &param, &property->value);
				}
				if (property->graph) {
					tracker_db_statement_bind_int (stmt, param++, ensure_resource_id (property->graph, NULL));
				} else {
					tracker_db_statement_bind_null (stmt, param++);
				}
			}

			tracker_db_statement_bind_int (stmt, param++, resource_buffer->id);

			tracker_db_statement_execute (stmt, &actual_error);
			g_object_unref (stmt);

			g_string_free (sql, TRUE);

			if (actual_error) {
				g_propagate_error (error, actual_error);
				return;
			}
		}
	}

	if (resource_buffer->fts_updated) {
		TrackerProperty *prop;
		GValueArray *values;

		tracker_fts_update_init (resource_buffer->id);

		g_hash_table_iter_init (&iter, resource_buffer->predicates);
		while (g_hash_table_iter_next (&iter, (gpointer*) &prop, (gpointer*) &values)) {
			if (tracker_property_get_fulltext_indexed (prop)) {
				fts = g_string_new ("");
				for (i = 0; i < values->n_values; i++) {
					g_string_append (fts, g_value_get_string (g_value_array_get_nth (values, i)));
					g_string_append_c (fts, ' ');
				}
				tracker_fts_update_text (resource_buffer->id,
							 tracker_data_query_resource_id (tracker_property_get_uri (prop)),
							 fts->str, !tracker_property_get_fulltext_no_limit (prop));
				g_string_free (fts, TRUE);
			}
		}
	}
}

static void resource_buffer_free (TrackerDataUpdateBufferResource *resource)
{
	g_free (resource->new_subject);
	resource->new_subject = NULL;

	g_hash_table_unref (resource->predicates);
	g_hash_table_unref (resource->tables);
	g_free (resource->subject);
	resource->subject = NULL;

	g_ptr_array_foreach (resource->types, (GFunc) g_free, NULL);
	g_ptr_array_free (resource->types, TRUE);
	resource->types = NULL;

	g_slice_free (TrackerDataUpdateBufferResource, resource);
}

void
tracker_data_update_buffer_flush (GError **error)
{
	GHashTableIter iter;
	GError *actual_error = NULL;

	g_hash_table_iter_init (&iter, update_buffer.resources);
	while (g_hash_table_iter_next (&iter, NULL, (gpointer*) &resource_buffer)) {
		tracker_data_resource_buffer_flush (&actual_error);
		if (actual_error) {
			g_propagate_error (error, actual_error);
			break;
		}
	}

	g_hash_table_remove_all (update_buffer.resources);
	resource_buffer = NULL;
}

static void
tracker_data_update_buffer_clear (void)
{
	g_hash_table_remove_all (update_buffer.resources);
	resource_buffer = NULL;

	tracker_fts_update_rollback ();
	update_buffer.fts_ever_updated = FALSE;
}

static void
tracker_data_blank_buffer_flush (GError **error)
{
	/* end of blank node */
	gint i;
	gint id;
	gchar *subject;
	gchar *blank_uri;
	const gchar *sha1;
	GChecksum *checksum;
	GError *actual_error = NULL;

	subject = blank_buffer.subject;
	blank_buffer.subject = NULL;

	/* we share anonymous blank nodes with identical properties
	   to avoid blowing up the database with duplicates */

	checksum = g_checksum_new (G_CHECKSUM_SHA1);

	/* generate hash uri from data to find resource
	   assumes no collisions due to generally little contents of anonymous nodes */
	for (i = 0; i < blank_buffer.predicates->len; i++) {
		g_checksum_update (checksum, g_array_index (blank_buffer.predicates, guchar *, i), -1);
		g_checksum_update (checksum, g_array_index (blank_buffer.objects, guchar *, i), -1);
	}

	sha1 = g_checksum_get_string (checksum);

	/* generate name based uuid */
	blank_uri = g_strdup_printf ("urn:uuid:%.8s-%.4s-%.4s-%.4s-%.12s",
	                             sha1, sha1 + 8, sha1 + 12, sha1 + 16, sha1 + 20);

	id = tracker_data_query_resource_id (blank_uri);

	if (id == 0) {
		/* uri not found
		   replay piled up statements to create resource */
		for (i = 0; i < blank_buffer.predicates->len; i++) {
			tracker_data_insert_statement (g_array_index (blank_buffer.graphs, gchar *, i),
			                               blank_uri,
			                               g_array_index (blank_buffer.predicates, gchar *, i),
			                               g_array_index (blank_buffer.objects, gchar *, i),
			                               &actual_error);
			if (actual_error) {
				break;
			}
		}
	}

	/* free piled up statements */
	for (i = 0; i < blank_buffer.predicates->len; i++) {
		g_free (g_array_index (blank_buffer.graphs, gchar *, i));
		g_free (g_array_index (blank_buffer.predicates, gchar *, i));
		g_free (g_array_index (blank_buffer.objects, gchar *, i));
	}
	g_array_remove_range (blank_buffer.graphs, 0, blank_buffer.graphs->len);
	g_array_remove_range (blank_buffer.predicates, 0, blank_buffer.predicates->len);
	g_array_remove_range (blank_buffer.objects, 0, blank_buffer.objects->len);

	g_hash_table_insert (blank_buffer.table, subject, blank_uri);
	g_checksum_free (checksum);

	if (actual_error) {
		g_propagate_error (error, actual_error);
	}
}

static void
cache_create_service_decomposed (TrackerClass *cl,
                                 const gchar  *graph)
{
	TrackerClass       **super_classes;
	GValue              gvalue = { 0 };
	gint                i;
	const gchar        *class_uri;

	/* also create instance of all super classes */
	super_classes = tracker_class_get_super_classes (cl);
	while (*super_classes) {
		cache_create_service_decomposed (*super_classes, graph);
		super_classes++;
	}

	class_uri = tracker_class_get_uri (cl);

	for (i = 0; i < resource_buffer->types->len; i++) {
		if (strcmp (g_ptr_array_index (resource_buffer->types, i), class_uri) == 0) {
			/* ignore duplicate statement */
			return;
		}
	}

	g_ptr_array_add (resource_buffer->types, g_strdup (class_uri));

	g_value_init (&gvalue, G_TYPE_INT);

	cache_insert_row (cl);

	g_value_set_int (&gvalue, ensure_resource_id (tracker_class_get_uri (cl), NULL));
	cache_insert_value ("rdfs:Resource_rdf:type", "rdf:type", &gvalue, graph, TRUE, FALSE, FALSE);

	tracker_class_set_count (cl, tracker_class_get_count (cl) + 1);
	if (insert_callbacks) {
		guint n;

		for (n = 0; n < insert_callbacks->len; n++) {
			TrackerStatementDelegate *delegate;

			delegate = g_ptr_array_index (insert_callbacks, n);
			delegate->callback (graph, resource_buffer->subject,
			                    RDF_PREFIX "type", class_uri,
			                    resource_buffer->types,
			                    delegate->user_data);
		}
	}
}

static gboolean
value_equal (GValue *value1,
             GValue *value2)
{
	GType type = G_VALUE_TYPE (value1);

	if (type != G_VALUE_TYPE (value2)) {
		return FALSE;
	}

	switch (type) {
	case G_TYPE_STRING:
		return (strcmp (g_value_get_string (value1), g_value_get_string (value2)) == 0);
	case G_TYPE_INT:
		return g_value_get_int (value1) == g_value_get_int (value2);
	case G_TYPE_DOUBLE:
		/* does RDF define equality for floating point values? */
		return g_value_get_double (value1) == g_value_get_double (value2);
	default:
		if (type == TRACKER_TYPE_DATE_TIME) {
			/* ignore UTC offset for comparison, irrelevant for comparison according to xsd:dateTime spec
			 * http://www.w3.org/TR/xmlschema-2/#dateTime
			 */
			return tracker_date_time_get_time (value1) == tracker_date_time_get_time (value2);
		}
		g_assert_not_reached ();
	}
}

static gboolean
value_set_add_value (GValueArray *value_set,
                     GValue      *value)
{
	gint i;

	g_return_val_if_fail (G_VALUE_TYPE (value), FALSE);

	for (i = 0; i < value_set->n_values; i++) {
		if (value_equal (g_value_array_get_nth (value_set, i), value)) {
			/* no change, value already in set */
			return FALSE;
		}
	}

	g_value_array_append (value_set, value);

	return TRUE;
}

static gboolean
value_set_remove_value (GValueArray *value_set,
                        GValue      *value)
{
	gint i;

	g_return_val_if_fail (G_VALUE_TYPE (value), FALSE);

	for (i = 0; i < value_set->n_values; i++) {
		if (value_equal (g_value_array_get_nth (value_set, i), value)) {
			/* value found, remove from set */

			g_value_array_remove (value_set, i);

			return TRUE;
		}
	}

	/* no change, value not found */
	return FALSE;
}

static gboolean
check_property_domain (TrackerProperty *property)
{
	gint type_index;

	for (type_index = 0; type_index < resource_buffer->types->len; type_index++) {
		if (strcmp (tracker_class_get_uri (tracker_property_get_domain (property)),
		            g_ptr_array_index (resource_buffer->types, type_index)) == 0) {
			return TRUE;
		}
	}
	return FALSE;
}

static GValueArray *
get_property_values (TrackerProperty *property)
{
	gboolean            multiple_values;
	GValueArray        *old_values;

	multiple_values = tracker_property_get_multiple_values (property);

	old_values = g_value_array_new (multiple_values ? 4 : 1);
	g_hash_table_insert (resource_buffer->predicates, g_object_ref (property), old_values);

	if (!resource_buffer->create) {
		TrackerDBInterface *iface;
		TrackerDBStatement *stmt;
		TrackerDBResultSet *result_set;
		gchar              *table_name;
		const gchar        *field_name;

		if (multiple_values) {
			table_name = g_strdup_printf ("%s_%s",
				                      tracker_class_get_name (tracker_property_get_domain (property)),
				                      tracker_property_get_name (property));
		} else {
			table_name = g_strdup (tracker_class_get_name (tracker_property_get_domain (property)));
		}
		field_name = tracker_property_get_name (property);

		iface = tracker_db_manager_get_db_interface ();

		stmt = tracker_db_interface_create_statement (iface, "SELECT \"%s\" FROM \"%s\" WHERE ID = ?", field_name, table_name);
		tracker_db_statement_bind_int (stmt, 0, resource_buffer->id);
		result_set = tracker_db_statement_execute (stmt, NULL);

		/* We use a result_set instead of a cursor here because it's
		 * possible that otherwise the cursor would remain open during
		 * the call from delete_resource_description. In future we want
		 * to allow having the same query open on multiple cursors,
		 * right now we don't support this. Which is why this workaround */

		if (result_set) {
			gboolean valid = TRUE;

			while (valid) {
				GValue gvalue = { 0 };
				_tracker_db_result_set_get_value (result_set, 0, &gvalue);
				if (G_VALUE_TYPE (&gvalue)) {
					g_value_array_append (old_values, &gvalue);
					g_value_unset (&gvalue);
				}
				valid = tracker_db_result_set_iter_next (result_set);
			}
			g_object_unref (result_set);
		}
		g_object_unref (stmt);

		g_free (table_name);
	}

	return old_values;
}

static GValueArray *
get_old_property_values (TrackerProperty  *property,
                         GError          **error)
{
	gboolean            fts;
	TrackerProperty   **properties, *prop;
	GValueArray        *old_values;

	fts = tracker_property_get_fulltext_indexed (property);

	/* read existing property values */
	old_values = g_hash_table_lookup (resource_buffer->predicates, property);
	if (old_values == NULL) {
		if (!check_property_domain (property)) {
			g_set_error (error, TRACKER_DATA_ERROR, TRACKER_DATA_ERROR_CONSTRAINT,
			             "Subject `%s' is not in domain `%s' of property `%s'",
			             resource_buffer->subject,
			             tracker_class_get_name (tracker_property_get_domain (property)),
			             tracker_property_get_name (property));
			return NULL;
		}

		if (fts && !resource_buffer->fts_updated && !resource_buffer->create) {
			guint i, n_props;

			/* first fulltext indexed property to be modified
			 * retrieve values of all fulltext indexed properties
			 */
			tracker_fts_update_init (resource_buffer->id);

			properties = tracker_ontology_get_properties (&n_props);

			for (i = 0; i < n_props; i++) {
				prop = properties[i];

				if (tracker_property_get_fulltext_indexed (prop)
				    && check_property_domain (prop)) {
					gint i;

					old_values = get_property_values (prop);

					/* delete old fts entries */
					for (i = 0; i < old_values->n_values; i++) {
						tracker_fts_update_text (resource_buffer->id, -1,
						                         g_value_get_string (g_value_array_get_nth (old_values, i)),
									 !tracker_property_get_fulltext_no_limit (prop));
					}
				}
			}

			update_buffer.fts_ever_updated = TRUE;

			old_values = g_hash_table_lookup (resource_buffer->predicates, property);
		} else {
			old_values = get_property_values (property);
		}

		if (fts) {
			resource_buffer->fts_updated = TRUE;
		}
	}

	return old_values;
}

static void
string_to_gvalue (const gchar         *value,
                  TrackerPropertyType  type,
                  GValue              *gvalue)
{
	gint object_id;

	switch (type) {
	case TRACKER_PROPERTY_TYPE_STRING:
		g_value_init (gvalue, G_TYPE_STRING);
		g_value_set_string (gvalue, value);
		break;
	case TRACKER_PROPERTY_TYPE_INTEGER:
		g_value_init (gvalue, G_TYPE_INT);
		g_value_set_int (gvalue, atoi (value));
		break;
	case TRACKER_PROPERTY_TYPE_BOOLEAN:
		/* use G_TYPE_INT to be compatible with value stored in DB
		   (important for value_equal function) */
		g_value_init (gvalue, G_TYPE_INT);
		g_value_set_int (gvalue, strcmp (value, "true") == 0);
		break;
	case TRACKER_PROPERTY_TYPE_DOUBLE:
		g_value_init (gvalue, G_TYPE_DOUBLE);
		g_value_set_double (gvalue, atof (value));
		break;
	case TRACKER_PROPERTY_TYPE_DATE:
	case TRACKER_PROPERTY_TYPE_DATETIME:
		g_value_init (gvalue, TRACKER_TYPE_DATE_TIME);
		tracker_date_time_set_from_string (gvalue, value);
		break;
	case TRACKER_PROPERTY_TYPE_RESOURCE:
		object_id = ensure_resource_id (value, NULL);
		g_value_init (gvalue, G_TYPE_INT);
		g_value_set_int (gvalue, object_id);
		break;
	default:
		g_warn_if_reached ();
		break;
	}
}

static void
cache_set_metadata_decomposed (TrackerProperty  *property,
                               const gchar      *value,
                               const gchar      *graph,
                               GError          **error)
{
	gboolean            multiple_values, fts;
	gchar              *table_name;
	const gchar        *field_name;
	TrackerProperty   **super_properties;
	GValue gvalue = { 0 };
	GValueArray        *old_values;

	/* also insert super property values */
	super_properties = tracker_property_get_super_properties (property);
	while (*super_properties) {
		cache_set_metadata_decomposed (*super_properties, value, graph, error);
		if (*error) {
			return;
		}
		super_properties++;
	}

	multiple_values = tracker_property_get_multiple_values (property);
	if (multiple_values) {
		table_name = g_strdup_printf ("%s_%s",
		                              tracker_class_get_name (tracker_property_get_domain (property)),
		                              tracker_property_get_name (property));
	} else {
		table_name = g_strdup (tracker_class_get_name (tracker_property_get_domain (property)));
	}
	field_name = tracker_property_get_name (property);

	fts = tracker_property_get_fulltext_indexed (property);

	/* read existing property values */
	old_values = get_old_property_values (property, error);
	if (*error) {
		g_free (table_name);
		return;
	}

	string_to_gvalue (value, tracker_property_get_data_type (property), &gvalue);

	if (!value_set_add_value (old_values, &gvalue)) {
		/* value already inserted */
		g_value_unset (&gvalue);
	} else if (!multiple_values && old_values->n_values > 1) {
		/* trying to add second value to single valued property */

		g_value_unset (&gvalue);

		g_set_error (error, TRACKER_DATA_ERROR, TRACKER_DATA_ERROR_CONSTRAINT,
		             "Unable to insert multiple values for subject `%s' and single valued property `%s'",
		             resource_buffer->subject,
		             field_name);
	} else {
		cache_insert_value (table_name, field_name, &gvalue, graph, multiple_values, fts,
		                    tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_DATETIME);
	}

	g_free (table_name);
}

static void
delete_metadata_decomposed (TrackerProperty  *property,
                            const gchar              *value,
                            GError          **error)
{
	gboolean            multiple_values, fts;
	gchar              *table_name;
	const gchar        *field_name;
	TrackerProperty   **super_properties;
	GValue gvalue = { 0 };
	GValueArray        *old_values;
	GError             *new_error = NULL;

	multiple_values = tracker_property_get_multiple_values (property);
	if (multiple_values) {
		table_name = g_strdup_printf ("%s_%s",
		                              tracker_class_get_name (tracker_property_get_domain (property)),
		                              tracker_property_get_name (property));
	} else {
		table_name = g_strdup (tracker_class_get_name (tracker_property_get_domain (property)));
	}
	field_name = tracker_property_get_name (property);

	fts = tracker_property_get_fulltext_indexed (property);

	/* read existing property values */
	old_values = get_old_property_values (property, &new_error);
	if (new_error) {
		g_free (table_name);
		g_propagate_error (error, new_error);
		return;
	}

	string_to_gvalue (value, tracker_property_get_data_type (property), &gvalue);

	if (!value_set_remove_value (old_values, &gvalue)) {
		/* value not found */
		g_value_unset (&gvalue);
	} else {
		cache_delete_value (table_name, field_name, &gvalue, multiple_values, fts,
		                    tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_DATETIME);
	}

	g_free (table_name);

	/* also delete super property values */
	super_properties = tracker_property_get_super_properties (property);
	while (*super_properties) {
		delete_metadata_decomposed (*super_properties, value, error);
		super_properties++;
	}
}

static void
cache_delete_resource_type (TrackerClass *class,
                            const gchar  *graph)
{
	TrackerDBInterface *iface;
	TrackerDBStatement *stmt;
	TrackerDBResultSet *result_set;
	TrackerProperty   **properties, *prop;
	gboolean            found;
	gint                i;
	guint               p, n_props;

	iface = tracker_db_manager_get_db_interface ();

	found = FALSE;
	for (i = 0; i < resource_buffer->types->len; i++) {
		if (strcmp (g_ptr_array_index (resource_buffer->types, i), tracker_class_get_uri (class)) == 0) {
			found = TRUE;
		}
	}

	if (!found) {
		/* type not found, nothing to do */
		return;
	}

	/* retrieve all subclasses we need to remove from the subject
	 * before we can remove the class specified as object of the statement */
	stmt = tracker_db_interface_create_statement (iface,
	                                              "SELECT (SELECT Uri FROM Resource WHERE ID = \"rdfs:Class_rdfs:subClassOf\".ID) "
	                                              "FROM \"rdfs:Resource_rdf:type\" INNER JOIN \"rdfs:Class_rdfs:subClassOf\" ON (\"rdf:type\" = \"rdfs:Class_rdfs:subClassOf\".ID) "
	                                              "WHERE \"rdfs:Resource_rdf:type\".ID = ? AND \"rdfs:subClassOf\" = (SELECT ID FROM Resource WHERE Uri = ?)");
	tracker_db_statement_bind_int (stmt, 0, resource_buffer->id);
	tracker_db_statement_bind_text (stmt, 1, tracker_class_get_uri (class));
	result_set = tracker_db_statement_execute (stmt, NULL);
	g_object_unref (stmt);

	if (result_set) {
		do {
			gchar *class_uri;

			tracker_db_result_set_get (result_set, 0, &class_uri, -1);
			cache_delete_resource_type (tracker_ontology_get_class_by_uri (class_uri), graph);
			g_free (class_uri);
		} while (tracker_db_result_set_iter_next (result_set));

		g_object_unref (result_set);
	}

	/* delete all property values */

	properties = tracker_ontology_get_properties (&n_props);

	for (p = 0; p < n_props; p++) {
		gboolean            multiple_values, fts;
		gchar              *table_name;
		const gchar        *field_name;
		GValueArray        *old_values;
		gint                i;

		prop = properties[p];

		if (tracker_property_get_domain (prop) != class) {
			continue;
		}

		multiple_values = tracker_property_get_multiple_values (prop);
		if (multiple_values) {
			table_name = g_strdup_printf ("%s_%s",
			                              tracker_class_get_name (class),
			                              tracker_property_get_name (prop));
		} else {
			table_name = g_strdup (tracker_class_get_name (class));
		}
		field_name = tracker_property_get_name (prop);

		fts = tracker_property_get_fulltext_indexed (prop);

		old_values = get_old_property_values (prop, NULL);

		for (i = old_values->n_values - 1; i >= 0 ; i--) {
			GValue *old_gvalue;
			GValue  gvalue = { 0 };

			old_gvalue = g_value_array_get_nth (old_values, i);
			g_value_init (&gvalue, G_VALUE_TYPE (old_gvalue));
			g_value_copy (old_gvalue, &gvalue);

			value_set_remove_value (old_values, &gvalue);
			cache_delete_value (table_name, field_name, &gvalue, multiple_values, fts,
			                    tracker_property_get_data_type (prop) == TRACKER_PROPERTY_TYPE_DATETIME);
		}

		g_free (table_name);
	}

	cache_delete_row (class);

	if (delete_callbacks) {
		guint n;
		for (n = 0; n < delete_callbacks->len; n++) {
			TrackerStatementDelegate *delegate;

			delegate = g_ptr_array_index (delete_callbacks, n);
			delegate->callback (graph, resource_buffer->subject,
			                    RDF_PREFIX "type", tracker_class_get_uri (class),
			                    resource_buffer->types, delegate->user_data);
		}
	}

}

void
tracker_data_delete_statement (const gchar  *graph,
                               const gchar  *subject,
                               const gchar  *predicate,
                               const gchar  *object,
                               GError      **error)
{
	TrackerClass       *class;
	TrackerProperty    *field;
	gint                subject_id;

	g_return_if_fail (subject != NULL);
	g_return_if_fail (predicate != NULL);
	g_return_if_fail (object != NULL);
	g_return_if_fail (in_transaction);

	subject_id = query_resource_id (subject);

	if (subject_id == 0) {
		/* subject not in database */
		return;
	}

	if (resource_buffer == NULL || strcmp (resource_buffer->subject, subject) != 0) {
		/* switch subject */
		resource_buffer = g_hash_table_lookup (update_buffer.resources, subject);
	}

	if (resource_buffer == NULL) {
		/* subject not yet in cache, retrieve or create ID */
		resource_buffer = g_slice_new0 (TrackerDataUpdateBufferResource);
		resource_buffer->subject = g_strdup (subject);
		resource_buffer->id = subject_id;
		resource_buffer->fts_updated = FALSE;
		resource_buffer->types = tracker_data_query_rdf_type (resource_buffer->id);
		resource_buffer->predicates = g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, (GDestroyNotify) g_value_array_free);
		resource_buffer->tables = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) cache_table_free);

		g_hash_table_insert (update_buffer.resources, g_strdup (subject), resource_buffer);
	}

	if (object && g_strcmp0 (predicate, RDF_PREFIX "type") == 0) {
		class = tracker_ontology_get_class_by_uri (object);
		if (class != NULL) {
			if (!in_journal_replay) {
				tracker_db_journal_append_delete_statement_id (
					(graph != NULL ? query_resource_id (graph) : 0),
					resource_buffer->id,
					tracker_data_query_resource_id (predicate),
					query_resource_id (object));
			}

			cache_delete_resource_type (class, graph);
		} else {
			g_set_error (error, TRACKER_DATA_ERROR, TRACKER_DATA_ERROR_UNKNOWN_CLASS,
			             "Class '%s' not found in the ontology", object);
		}
	} else {
		field = tracker_ontology_get_property_by_uri (predicate);
		if (field != NULL) {
			gint id = tracker_property_get_id (field);
			if (!in_journal_replay) {
				if (tracker_property_get_data_type (field) == TRACKER_PROPERTY_TYPE_RESOURCE) {
					tracker_db_journal_append_delete_statement_id (
						(graph != NULL ? query_resource_id (graph) : 0),
						resource_buffer->id,
						(id != 0) ? id : tracker_data_query_resource_id (predicate),
						query_resource_id (object));
				} else {
					tracker_db_journal_append_delete_statement (
						(graph != NULL ? query_resource_id (graph) : 0),
						resource_buffer->id,
						(id != 0) ? id : tracker_data_query_resource_id (predicate),
						object);
				}
			}

			delete_metadata_decomposed (field, object, error);
		} else {
			g_set_error (error, TRACKER_DATA_ERROR, TRACKER_DATA_ERROR_UNKNOWN_PROPERTY,
			             "Property '%s' not found in the ontology", predicate);
		}

		if (delete_callbacks) {
			guint n;
			for (n = 0; n < delete_callbacks->len; n++) {
				TrackerStatementDelegate *delegate;

				delegate = g_ptr_array_index (delete_callbacks, n);
				delegate->callback (graph, subject, predicate, object,
				                    resource_buffer->types,
				                    delegate->user_data);
			}
		}
	}
}

static gboolean
tracker_data_insert_statement_common (const gchar            *graph,
                                      const gchar            *subject,
                                      const gchar            *predicate,
                                      const gchar            *object,
                                      GError                **error)
{
	if (g_str_has_prefix (subject, ":")) {
		/* blank node definition
		   pile up statements until the end of the blank node */
		gchar *value;
		GError *actual_error = NULL;

		if (blank_buffer.subject != NULL) {
			/* active subject in buffer */
			if (strcmp (blank_buffer.subject, subject) != 0) {
				/* subject changed, need to flush buffer */
				tracker_data_blank_buffer_flush (&actual_error);

				if (actual_error) {
					g_propagate_error (error, actual_error);
					return FALSE;
				}
			}
		}

		if (blank_buffer.subject == NULL) {
			blank_buffer.subject = g_strdup (subject);
			if (blank_buffer.graphs == NULL) {
				blank_buffer.graphs = g_array_sized_new (FALSE, FALSE, sizeof (char*), 4);
				blank_buffer.predicates = g_array_sized_new (FALSE, FALSE, sizeof (char*), 4);
				blank_buffer.objects = g_array_sized_new (FALSE, FALSE, sizeof (char*), 4);
			}
		}

		value = g_strdup (graph);
		g_array_append_val (blank_buffer.graphs, value);
		value = g_strdup (predicate);
		g_array_append_val (blank_buffer.predicates, value);
		value = g_strdup (object);
		g_array_append_val (blank_buffer.objects, value);

		return FALSE;
	}

	if (resource_buffer == NULL || strcmp (resource_buffer->subject, subject) != 0) {
		/* switch subject */
		resource_buffer = g_hash_table_lookup (update_buffer.resources, subject);
	}

	if (resource_buffer == NULL) {
		GValue gvalue = { 0 };

		g_value_init (&gvalue, G_TYPE_INT);

		/* subject not yet in cache, retrieve or create ID */
		resource_buffer = g_slice_new0 (TrackerDataUpdateBufferResource);
		resource_buffer->subject = g_strdup (subject);
		resource_buffer->id = ensure_resource_id (resource_buffer->subject, &resource_buffer->create);
		resource_buffer->fts_updated = FALSE;
		if (resource_buffer->create) {
			resource_buffer->types = g_ptr_array_new ();
		} else {
			resource_buffer->types = tracker_data_query_rdf_type (resource_buffer->id);
		}
		resource_buffer->predicates = g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, (GDestroyNotify) g_value_array_free);
		resource_buffer->tables = g_hash_table_new_full (g_str_hash, g_str_equal,
		                                                 g_free, (GDestroyNotify) cache_table_free);

		g_value_set_int (&gvalue, tracker_data_update_get_next_modseq ());
		cache_insert_value ("rdfs:Resource", "tracker:modified", &gvalue, graph, FALSE, FALSE, FALSE);

		g_hash_table_insert (update_buffer.resources, g_strdup (subject), resource_buffer);
	}

	return TRUE;
}

void
tracker_data_insert_statement (const gchar            *graph,
                               const gchar            *subject,
                               const gchar            *predicate,
                               const gchar            *object,
                               GError                **error)
{
	TrackerProperty *property;

	g_return_if_fail (subject != NULL);
	g_return_if_fail (predicate != NULL);
	g_return_if_fail (object != NULL);
	g_return_if_fail (in_transaction);

	property = tracker_ontology_get_property_by_uri (predicate);
	if (property != NULL) {
		if (tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_RESOURCE) {
			tracker_data_insert_statement_with_uri (graph, subject, predicate, object, error);
		} else {
			tracker_data_insert_statement_with_string (graph, subject, predicate, object, error);
		}
	} else if (strcmp (predicate, TRACKER_PREFIX "uri") == 0) {
		tracker_data_insert_statement_with_uri (graph, subject, predicate, object, error);
	} else {
		g_set_error (error, TRACKER_DATA_ERROR, TRACKER_DATA_ERROR_UNKNOWN_PROPERTY,
		             "Property '%s' not found in the ontology", predicate);
	}
}

void
tracker_data_insert_statement_with_uri (const gchar            *graph,
                                        const gchar            *subject,
                                        const gchar            *predicate,
                                        const gchar            *object,
                                        GError                **error)
{
	GError          *actual_error = NULL;
	TrackerClass    *class;
	TrackerProperty *property;
	gint             prop_id = 0;

	g_return_if_fail (subject != NULL);
	g_return_if_fail (predicate != NULL);
	g_return_if_fail (object != NULL);
	g_return_if_fail (in_transaction);

	property = tracker_ontology_get_property_by_uri (predicate);
	if (property == NULL) {
		if (strcmp (predicate, TRACKER_PREFIX "uri") == 0) {
			/* virtual tracker:uri property */
		} else {
			g_set_error (error, TRACKER_DATA_ERROR, TRACKER_DATA_ERROR_UNKNOWN_PROPERTY,
			             "Property '%s' not found in the ontology", predicate);
			return;
		}
	} else {
		if (tracker_property_get_data_type (property) != TRACKER_PROPERTY_TYPE_RESOURCE) {
			g_set_error (error, TRACKER_DATA_ERROR, TRACKER_DATA_ERROR_INVALID_TYPE,
			             "Property '%s' does not accept URIs", predicate);
			return;
		}
		prop_id = tracker_property_get_id (property);
	}

	/* subjects and objects starting with `:' are anonymous blank nodes */
	if (g_str_has_prefix (object, ":")) {
		/* anonymous blank node used as object in a statement */
		const gchar *blank_uri;

		if (blank_buffer.subject != NULL) {
			if (strcmp (blank_buffer.subject, object) == 0) {
				/* object still in blank buffer, need to flush buffer */
				tracker_data_blank_buffer_flush (&actual_error);

				if (actual_error) {
					g_propagate_error (error, actual_error);
					return;
				}
			}
		}

		blank_uri = g_hash_table_lookup (blank_buffer.table, object);

		if (blank_uri != NULL) {
			/* now insert statement referring to blank node */
			tracker_data_insert_statement (graph, subject, predicate, blank_uri, &actual_error);

			g_hash_table_remove (blank_buffer.table, object);

			if (actual_error) {
				g_propagate_error (error, actual_error);
				return;
			}

			return;
		} else {
			g_critical ("Blank node '%s' not found", object);
		}
	}

	if (!tracker_data_insert_statement_common (graph, subject, predicate, object, &actual_error)) {
		if (actual_error) {
			g_propagate_error (error, actual_error);
			return;
		}

		return;
	}

	if (strcmp (predicate, RDF_PREFIX "type") == 0) {
		/* handle rdf:type statements specially to
		   cope with inference and insert blank rows */
		class = tracker_ontology_get_class_by_uri (object);
		if (class != NULL) {
			cache_create_service_decomposed (class, graph);
		} else {
			g_set_error (error, TRACKER_DATA_ERROR, TRACKER_DATA_ERROR_UNKNOWN_CLASS,
			             "Class '%s' not found in the ontology", object);
			return;
		}
	} else if (strcmp (predicate, TRACKER_PREFIX "uri") == 0) {
		/* internal property tracker:uri, used to change uri of existing element */
		resource_buffer->new_subject = g_strdup (object);
	} else {
		/* add value to metadata database */
		cache_set_metadata_decomposed (property, object, graph, &actual_error);
		if (actual_error) {
			tracker_data_update_buffer_clear ();
			g_propagate_error (error, actual_error);
			return;
		}

		if (insert_callbacks) {
			guint n;
			for (n = 0; n < insert_callbacks->len; n++) {
				TrackerStatementDelegate *delegate;

				delegate = g_ptr_array_index (insert_callbacks, n);
				delegate->callback (graph, subject, predicate, object,
				                    resource_buffer->types,
				                    delegate->user_data);
			}
		}
	}

	if (!in_journal_replay) {
		tracker_db_journal_append_insert_statement_id (
			(graph != NULL ? query_resource_id (graph) : 0),
			resource_buffer->id,
			(prop_id != 0) ? prop_id : tracker_data_query_resource_id (predicate),
			query_resource_id (object));
	}
}

void
tracker_data_insert_statement_with_string (const gchar            *graph,
                                           const gchar            *subject,
                                           const gchar            *predicate,
                                           const gchar            *object,
                                           GError                **error)
{
	GError          *actual_error = NULL;
	TrackerProperty *property;
	gint             id = 0;

	g_return_if_fail (subject != NULL);
	g_return_if_fail (predicate != NULL);
	g_return_if_fail (object != NULL);
	g_return_if_fail (in_transaction);

	property = tracker_ontology_get_property_by_uri (predicate);
	if (property == NULL) {
		g_set_error (error, TRACKER_DATA_ERROR, TRACKER_DATA_ERROR_UNKNOWN_PROPERTY,
		             "Property '%s' not found in the ontology", predicate);
		return;
	} else {
		if (tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_RESOURCE) {
			g_set_error (error, TRACKER_DATA_ERROR, TRACKER_DATA_ERROR_INVALID_TYPE,
			             "Property '%s' only accepts URIs", predicate);
			return;
		}
		id = tracker_property_get_id (property);
	}

	if (!tracker_data_insert_statement_common (graph, subject, predicate, object, &actual_error)) {
		if (actual_error) {
			g_propagate_error (error, actual_error);
			return;
		}

		return;
	}

	/* add value to metadata database */
	cache_set_metadata_decomposed (property, object, graph, &actual_error);
	if (actual_error) {
		tracker_data_update_buffer_clear ();
		g_propagate_error (error, actual_error);
		return;
	}

	if (insert_callbacks) {
		guint n;
		for (n = 0; n < insert_callbacks->len; n++) {
			TrackerStatementDelegate *delegate;

			delegate = g_ptr_array_index (insert_callbacks, n);
			delegate->callback (graph, subject, predicate, object,
			                    resource_buffer->types,
			                    delegate->user_data);
		}
	}

	if (!in_journal_replay) {
		tracker_db_journal_append_insert_statement (
			(graph != NULL ? query_resource_id (graph) : 0),
			resource_buffer->id,
			(id != 0) ? id : tracker_data_query_resource_id (predicate),
			object);
	}
}

static void
db_set_volume_available (const gchar *uri,
                         gboolean     available)
{
	TrackerDBInterface *iface;
	TrackerDBStatement *stmt;

	iface = tracker_db_manager_get_db_interface ();

	stmt = tracker_db_interface_create_statement (iface, "UPDATE \"rdfs:Resource\" SET Available = ? WHERE ID IN (SELECT ID FROM \"nie:DataObject\" WHERE \"nie:dataSource\" IN (SELECT ID FROM Resource WHERE Uri = ?))");
	tracker_db_statement_bind_int (stmt, 0, available ? 1 : 0);
	tracker_db_statement_bind_text (stmt, 1, uri);
	tracker_db_statement_execute (stmt, NULL);
	g_object_unref (stmt);
}

void
tracker_data_update_enable_volume (const gchar *udi,
                                   const gchar *mount_path)
{
	gchar              *removable_device_urn;
	gchar *delete_q;
	gchar *set_q;
	gchar *mount_path_uri;
	GFile *mount_path_file;
	GError *error = NULL;

	g_return_if_fail (udi != NULL);
	g_return_if_fail (mount_path != NULL);

	removable_device_urn = g_strdup_printf (TRACKER_DATASOURCE_URN_PREFIX "%s", udi);

	db_set_volume_available (removable_device_urn, TRUE);

	mount_path_file = g_file_new_for_path (mount_path);
	mount_path_uri = g_file_get_uri (mount_path_file);

	delete_q = g_strdup_printf ("DELETE FROM <%s> { <%s> tracker:mountPoint ?d } WHERE { <%s> tracker:mountPoint ?d }",
	                            removable_device_urn, removable_device_urn, removable_device_urn);
	set_q = g_strdup_printf ("INSERT INTO <%s> { <%s> a tracker:Volume; tracker:mountPoint <%s> }",
	                         removable_device_urn, removable_device_urn, mount_path_uri);

	tracker_data_update_sparql (delete_q, &error);

	if (error) {
		g_critical ("%s", error->message);
		g_error_free (error);
		error = NULL;
	}

	tracker_data_update_sparql (set_q, &error);

	if (error) {
		g_critical ("%s", error->message);
		g_error_free (error);
	}

	g_free (set_q);
	g_free (delete_q);

	delete_q = g_strdup_printf ("DELETE FROM <%s> { <%s> tracker:isMounted ?d } WHERE { <%s> tracker:isMounted ?d }",
	                            removable_device_urn, removable_device_urn, removable_device_urn);
	set_q = g_strdup_printf ("INSERT INTO <%s> { <%s> a tracker:Volume; tracker:isMounted true }",
	                         removable_device_urn, removable_device_urn);

	tracker_data_update_sparql (delete_q, &error);

	if (error) {
		g_critical ("%s", error->message);
		g_error_free (error);
		error = NULL;
	}

	tracker_data_update_sparql (set_q, &error);

	if (error) {
		g_critical ("%s", error->message);
		g_error_free (error);
	}

	g_free (set_q);
	g_free (delete_q);

	g_free (mount_path_uri);
	g_object_unref (mount_path_file);
	g_free (removable_device_urn);
}

void
tracker_data_update_reset_volume (const gchar *uri)
{
	time_t mnow;
	gchar *now_as_string;
	gchar *delete_q;
	gchar *set_q;
	GError *error = NULL;

	mnow = time (NULL);
	now_as_string = tracker_date_to_string (mnow);
	delete_q = g_strdup_printf ("DELETE FROM <%s> { <%s> tracker:unmountDate ?d } WHERE { <%s> tracker:unmountDate ?d }",
	                            uri, uri, uri);
	set_q = g_strdup_printf ("INSERT INTO <%s> { <%s> a tracker:Volume; tracker:unmountDate \"%s\" }",
	                         uri, uri, now_as_string);

	tracker_data_update_sparql (delete_q, &error);

	if (error) {
		g_critical ("%s", error->message);
		g_error_free (error);
		error = NULL;
	}

	tracker_data_update_sparql (set_q, &error);

	if (error) {
		g_critical ("%s", error->message);
		g_error_free (error);
	}

	g_free (now_as_string);
	g_free (set_q);
	g_free (delete_q);
}

void
tracker_data_update_disable_volume (const gchar *udi)
{
	gchar *removable_device_urn;
	gchar *delete_q;
	gchar *set_q;
	GError *error = NULL;

	g_return_if_fail (udi != NULL);

	removable_device_urn = g_strdup_printf (TRACKER_DATASOURCE_URN_PREFIX "%s", udi);

	db_set_volume_available (removable_device_urn, FALSE);

	tracker_data_update_reset_volume (removable_device_urn);

	delete_q = g_strdup_printf ("DELETE FROM <%s> { <%s> tracker:isMounted ?d } WHERE { <%s> tracker:isMounted ?d }",
	                            removable_device_urn, removable_device_urn, removable_device_urn);
	set_q = g_strdup_printf ("INSERT INTO <%s> { <%s> a tracker:Volume; tracker:isMounted false }",
	                         removable_device_urn, removable_device_urn);

	tracker_data_update_sparql (delete_q, &error);

	if (error) {
		g_critical ("%s", error->message);
		g_error_free (error);
		error = NULL;
	}

	tracker_data_update_sparql (set_q, &error);

	if (error) {
		g_critical ("%s", error->message);
		g_error_free (error);
	}

	g_free (set_q);
	g_free (delete_q);

	g_free (removable_device_urn);
}

void
tracker_data_update_disable_all_volumes (void)
{
	TrackerDBInterface *iface;
	TrackerDBStatement *stmt;
	gchar *delete_q, *set_q;
	GError *error = NULL;

	iface = tracker_db_manager_get_db_interface ();

	stmt = tracker_db_interface_create_statement (iface,
	                                              "UPDATE \"rdfs:Resource\" SET Available = 0 "
	                                              "WHERE ID IN ("
	                                              "SELECT ID FROM \"nie:DataObject\" "
	                                              "WHERE \"nie:dataSource\" IN ("
	                                              "SELECT ID FROM Resource WHERE Uri != ?"
	                                              ")"
	                                              ")");
	tracker_db_statement_bind_text (stmt, 0, TRACKER_NON_REMOVABLE_MEDIA_DATASOURCE_URN);
	tracker_db_statement_execute (stmt, NULL);
	g_object_unref (stmt);

	/* The INTO and FROM uris are not really right, but it should be harmless:
	 * we just want graph to be != NULL in tracker-store/tracker-writeback.c */

	delete_q = g_strdup_printf ("DELETE FROM <"TRACKER_NON_REMOVABLE_MEDIA_DATASOURCE_URN"> { ?o tracker:isMounted ?d } WHERE { ?o tracker:isMounted ?d  FILTER (?o != <"TRACKER_NON_REMOVABLE_MEDIA_DATASOURCE_URN"> ) }");
	set_q = g_strdup_printf ("INSERT INTO <"TRACKER_NON_REMOVABLE_MEDIA_DATASOURCE_URN"> { ?o a tracker:Volume; tracker:isMounted false } WHERE { ?o a tracker:Volume FILTER (?o != <"TRACKER_NON_REMOVABLE_MEDIA_DATASOURCE_URN"> ) }");

	tracker_data_update_sparql (delete_q, &error);

	if (error) {
		g_critical ("%s", error->message);
		g_error_free (error);
		error = NULL;
	}

	tracker_data_update_sparql (set_q, &error);

	if (error) {
		g_critical ("%s", error->message);
		g_error_free (error);
	}

	g_free (set_q);
	g_free (delete_q);
}

void
tracker_data_begin_transaction (void)
{
	TrackerDBInterface *iface;

	g_return_if_fail (!in_transaction);

	resource_time = time (NULL);

	if (update_buffer.resource_cache == NULL) {
		update_buffer.resource_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
		update_buffer.resources = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) resource_buffer_free);
	}

	resource_buffer = NULL;
	if (blank_buffer.table == NULL) {
		blank_buffer.table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	}

	iface = tracker_db_manager_get_db_interface ();

	tracker_db_interface_start_transaction (iface);

	in_transaction = TRUE;
}

void
tracker_data_begin_replay_transaction (time_t time)
{
	in_journal_replay = TRUE;
	tracker_data_begin_transaction ();
	resource_time = time;
}

void
tracker_data_commit_transaction (void)
{
	TrackerDBInterface *iface;

	g_return_if_fail (in_transaction);

	in_transaction = FALSE;

	tracker_data_update_buffer_flush (NULL);

	if (update_buffer.fts_ever_updated) {
		tracker_fts_update_commit ();
		update_buffer.fts_ever_updated = FALSE;
	}

	iface = tracker_db_manager_get_db_interface ();

	tracker_db_interface_end_transaction (iface);

	g_hash_table_remove_all (update_buffer.resources);
	g_hash_table_remove_all (update_buffer.resource_cache);

	if (commit_callbacks) {
		guint n;
		for (n = 0; n < commit_callbacks->len; n++) {
			TrackerCommitDelegate *delegate;
			delegate = g_ptr_array_index (commit_callbacks, n);
			delegate->callback (delegate->user_data);
		}
	}

	in_journal_replay = FALSE;
}

static void
format_sql_value_as_string (GString         *sql,
                            TrackerProperty *property)
{
	switch (tracker_property_get_data_type (property)) {
	case TRACKER_PROPERTY_TYPE_RESOURCE:
		g_string_append_printf (sql, "(SELECT Uri FROM Resource WHERE ID = \"%s\")", tracker_property_get_name (property));
		break;
	case TRACKER_PROPERTY_TYPE_INTEGER:
	case TRACKER_PROPERTY_TYPE_DOUBLE:
		g_string_append_printf (sql, "CAST (\"%s\" AS TEXT)", tracker_property_get_name (property));
		break;
	case TRACKER_PROPERTY_TYPE_BOOLEAN:
		g_string_append_printf (sql, "CASE \"%s\" WHEN 1 THEN 'true' WHEN 0 THEN 'false' ELSE NULL END", tracker_property_get_name (property));
		break;
	case TRACKER_PROPERTY_TYPE_DATETIME:
		g_string_append_printf (sql, "strftime (\"%%Y-%%m-%%dT%%H:%%M:%%SZ\", \"%s\", \"unixepoch\")", tracker_property_get_name (property));
		break;
	default:
		g_string_append_printf (sql, "\"%s\"", tracker_property_get_name (property));
		break;
	}
}

/**
 * Removes the description of a resource (embedded metadata), but keeps
 * annotations (non-embedded/user metadata) stored about the resource.
 */
void
tracker_data_delete_resource_description (const gchar *graph,
                                          const gchar *url,
                                          GError **error)
{
	TrackerDBInterface *iface;
	TrackerDBStatement *stmt;
	TrackerDBResultSet *result_set, *single_result, *multi_result;
	TrackerClass       *class;
	gchar              *urn;
	GString            *sql;
	TrackerProperty   **properties, *property;
	int                 i;
	gboolean            first, bail_out = FALSE;
	gint                resource_id;
	guint               p, n_props;

	/* We use result_sets instead of cursors here because it's possible
	 * that otherwise the query of the outer cursor would be reused by the
	 * cursors of the inner queries. */

	iface = tracker_db_manager_get_db_interface ();

	/* DROP GRAPH <url> - url here is nie:url */

	stmt = tracker_db_interface_create_statement (iface, "SELECT ID, (SELECT Uri FROM Resource WHERE ID = \"nie:DataObject\".ID) FROM \"nie:DataObject\" WHERE \"nie:DataObject\".\"nie:url\" = ?");
	tracker_db_statement_bind_text (stmt, 0, url);
	result_set = tracker_db_statement_execute (stmt, NULL);
	g_object_unref (stmt);

	if (result_set) {
		tracker_db_result_set_get (result_set, 0, &resource_id, -1);
		tracker_db_result_set_get (result_set, 1, &urn, -1);
		g_object_unref (result_set);
	} else {
		/* For fallback to the old behaviour, we could do this here:
		 * resource_id = tracker_data_query_resource_id (url); */
		return;
	}

	properties = tracker_ontology_get_properties (&n_props);

	stmt = tracker_db_interface_create_statement (iface, "SELECT (SELECT Uri FROM Resource WHERE ID = \"rdf:type\") FROM \"rdfs:Resource_rdf:type\" WHERE ID = ?");
	tracker_db_statement_bind_int (stmt, 0, resource_id);
	result_set = tracker_db_statement_execute (stmt, NULL);
	g_object_unref (stmt);

	if (result_set) {
		do {
			gchar *class_uri;

			tracker_db_result_set_get (result_set, 0, &class_uri, -1);

			class = tracker_ontology_get_class_by_uri (class_uri);

			if (class == NULL) {
				g_warning ("Class '%s' not found in the ontology", class_uri);
				g_free (class_uri);
				continue;
			}
			g_free (class_uri);

			/* retrieve single value properties for current class */

			sql = g_string_new ("SELECT ");

			first = TRUE;

			for (p = 0; p < n_props; p++) {
				property = properties[p];

				if (tracker_property_get_domain (property) == class) {
					if (!tracker_property_get_embedded (property)) {
						continue;
					}

					if (!tracker_property_get_multiple_values (property)) {
						if (!first) {
							g_string_append (sql, ", ");
						}
						first = FALSE;

						format_sql_value_as_string (sql, property);
					}
				}
			}

			single_result = NULL;
			if (!first) {
				g_string_append_printf (sql, " FROM \"%s\" WHERE ID = ?", tracker_class_get_name (class));
				stmt = tracker_db_interface_create_statement (iface, "%s", sql->str);
				tracker_db_statement_bind_int (stmt, 0, resource_id);
				single_result = tracker_db_statement_execute (stmt, NULL);
				g_object_unref (stmt);
			}

			g_string_free (sql, TRUE);

			i = 0;
			for (p = 0; p < n_props; p++) {
				property = properties[p];

				if (tracker_property_get_domain (property) != class) {
					continue;
				}

				if (!tracker_property_get_embedded (property)) {
					continue;
				}

				if (strcmp (tracker_property_get_uri (property), RDF_PREFIX "type") == 0) {
					/* Do not delete rdf:type statements */
					continue;
				}

				if (!tracker_property_get_multiple_values (property)) {
					gchar *value;
					GError *new_error = NULL;

					/* single value property, value in single_result_set */

					tracker_db_result_set_get (single_result, i++, &value, -1);

					if (value) {
						tracker_data_delete_statement (graph, urn,
						                               tracker_property_get_uri (property),
						                               value,
						                               &new_error);
						if (new_error) {
							g_propagate_error (error, new_error);
							bail_out = TRUE;
						}
						g_free (value);
					}

				} else {
					/* multi value property, retrieve values from DB */

					sql = g_string_new ("SELECT ");

					format_sql_value_as_string (sql, property);

					g_string_append_printf (sql,
					                        " FROM \"%s_%s\" WHERE ID = ?",
					                        tracker_class_get_name (tracker_property_get_domain (property)),
					                        tracker_property_get_name (property));

					stmt = tracker_db_interface_create_statement (iface, "%s", sql->str);
					tracker_db_statement_bind_int (stmt, 0, resource_id);
					multi_result = tracker_db_statement_execute (stmt, NULL);
					g_object_unref (stmt);

					if (multi_result) {
						do {
							gchar *value;
							GError *new_error = NULL;

							tracker_db_result_set_get (multi_result, 0, &value, -1);

							tracker_data_delete_statement (graph, urn,
							                               tracker_property_get_uri (property),
							                               value,
							                               &new_error);

							g_free (value);

							if (new_error) {
								g_propagate_error (error, new_error);
								bail_out = TRUE;
								break;
							}

						} while (tracker_db_result_set_iter_next (multi_result));

						g_object_unref (multi_result);
					}

					g_string_free (sql, TRUE);
				}
			}

			if (!first) {
				g_object_unref (single_result);
			}

		} while (!bail_out && tracker_db_result_set_iter_next (result_set));

		g_object_unref (result_set);
	}

	g_free (urn);
}


void
tracker_data_update_sparql (const gchar  *update,
                            GError      **error)
{
	GError *actual_error = NULL;
	TrackerDBInterface *iface;
	TrackerSparqlQuery *sparql_query;

	g_return_if_fail (update != NULL);

	iface = tracker_db_manager_get_db_interface ();

	sparql_query = tracker_sparql_query_new_update (update);
	resource_time = time (NULL);
	tracker_db_interface_execute_query (iface, NULL, "SAVEPOINT sparql");
	tracker_db_journal_start_transaction (resource_time);

	tracker_sparql_query_execute_update (sparql_query, FALSE, &actual_error);

	if (actual_error) {
		tracker_data_update_buffer_clear ();
		tracker_db_interface_execute_query (iface, NULL, "ROLLBACK TO sparql");
		tracker_db_journal_rollback_transaction ();

		if (rollback_callbacks) {
			guint n;
			for (n = 0; n < rollback_callbacks->len; n++) {
				TrackerCommitDelegate *delegate;
				delegate = g_ptr_array_index (rollback_callbacks, n);
				delegate->callback (delegate->user_data);
			}
		}

		g_propagate_error (error, actual_error);

		g_object_unref (sparql_query);
		return;
	}

	tracker_db_journal_commit_transaction ();
	resource_time = 0;
	tracker_db_interface_execute_query (iface, NULL, "RELEASE sparql");

	g_object_unref (sparql_query);
}


GPtrArray *
tracker_data_update_sparql_blank (const gchar  *update,
                                  GError      **error)
{
	GError *actual_error = NULL;
	TrackerDBInterface *iface;
	TrackerSparqlQuery *sparql_query;
	GPtrArray *blank_nodes;

	g_return_val_if_fail (update != NULL, NULL);

	iface = tracker_db_manager_get_db_interface ();

	sparql_query = tracker_sparql_query_new_update (update);

	resource_time = time (NULL);
	tracker_db_interface_execute_query (iface, NULL, "SAVEPOINT sparql");
	tracker_db_journal_start_transaction (resource_time);

	blank_nodes = tracker_sparql_query_execute_update (sparql_query, TRUE, &actual_error);

	if (actual_error) {
		tracker_data_update_buffer_clear ();
		tracker_db_interface_execute_query (iface, NULL, "ROLLBACK TO sparql");
		tracker_db_journal_rollback_transaction ();

		if (rollback_callbacks) {
			guint n;
			for (n = 0; n < rollback_callbacks->len; n++) {
				TrackerCommitDelegate *delegate;
				delegate = g_ptr_array_index (rollback_callbacks, n);
				delegate->callback (delegate->user_data);
			}
		}

		g_propagate_error (error, actual_error);

		g_object_unref (sparql_query);
		return NULL;
	}

	tracker_db_journal_commit_transaction ();
	resource_time = 0;
	tracker_db_interface_execute_query (iface, NULL, "RELEASE sparql");

	g_object_unref (sparql_query);

	return blank_nodes;
}

void
tracker_data_sync (void)
{
	tracker_db_journal_fsync ();
}


/*
 * Copyright (C) 2006, Mr Jamie McCracken (jamiemcc@gnome.org)
 * Copyright (C) 2007, Jason Kivlighn (jkivlighn@gmail.com)
 * Copyright (C) 2007, Creative Commons (http://creativecommons.org)
 * Copyright (C) 2008, Nokia
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
#include <fcntl.h>
#include <zlib.h>
#include <inttypes.h>

#include <glib/gstdio.h>

#include <libtracker-common/tracker-common.h>

#include <libtracker-fts/tracker-fts.h>

#include <libtracker-db/tracker-db-interface-sqlite.h>
#include <libtracker-db/tracker-db-manager.h>
#include <libtracker-db/tracker-db-journal.h>

#include "tracker-data-manager.h"
#include "tracker-data-update.h"
#include "tracker-sparql-query.h"

#define RDF_PREFIX TRACKER_RDF_PREFIX
#define RDF_PROPERTY RDF_PREFIX "Property"
#define RDF_TYPE RDF_PREFIX "type"

#define RDFS_PREFIX TRACKER_RDFS_PREFIX
#define RDFS_CLASS RDFS_PREFIX "Class"
#define RDFS_DOMAIN RDFS_PREFIX "domain"
#define RDFS_RANGE RDFS_PREFIX "range"
#define RDFS_SUB_CLASS_OF RDFS_PREFIX "subClassOf"
#define RDFS_SUB_PROPERTY_OF RDFS_PREFIX "subPropertyOf"

#define NRL_PREFIX TRACKER_NRL_PREFIX
#define NRL_INVERSE_FUNCTIONAL_PROPERTY TRACKER_NRL_PREFIX "InverseFunctionalProperty"
#define NRL_MAX_CARDINALITY NRL_PREFIX "maxCardinality"

#define TRACKER_PREFIX TRACKER_TRACKER_PREFIX

#define ZLIBBUFSIZ 8192

static gchar              *ontologies_dir;
static gboolean            initialized;
static gboolean            in_journal_replay;

static void
load_ontology_statement (const gchar *ontology_file,
                         gint         subject_id,
                         const gchar *subject,
                         const gchar *predicate,
                         const gchar *object,
                         gint        *max_id)
{
	if (g_strcmp0 (predicate, RDF_TYPE) == 0) {
		if (g_strcmp0 (object, RDFS_CLASS) == 0) {
			TrackerClass *class;

			if (tracker_ontology_get_class_by_uri (subject) != NULL) {
				g_critical ("%s: Duplicate definition of class %s", ontology_file, subject);
				return;
			}

			if (max_id) {
				subject_id = ++(*max_id);
			}

			class = tracker_class_new ();
			tracker_class_set_uri (class, subject);
			tracker_class_set_id (class, subject_id);
			tracker_ontology_add_class (class);
			tracker_ontology_add_id_uri_pair (subject_id, subject);
			g_object_unref (class);
		} else if (g_strcmp0 (object, RDF_PROPERTY) == 0) {
			TrackerProperty *property;

			if (tracker_ontology_get_property_by_uri (subject) != NULL) {
				g_critical ("%s: Duplicate definition of property %s", ontology_file, subject);
				return;
			}

			if (max_id) {
				subject_id = ++(*max_id);
			}

			property = tracker_property_new ();
			tracker_property_set_uri (property, subject);
			tracker_property_set_id (property, subject_id);
			tracker_ontology_add_property (property);
			tracker_ontology_add_id_uri_pair (subject_id, subject);
			g_object_unref (property);
		} else if (g_strcmp0 (object, NRL_INVERSE_FUNCTIONAL_PROPERTY) == 0) {
			TrackerProperty *property;

			property = tracker_ontology_get_property_by_uri (subject);
			if (property == NULL) {
				g_critical ("%s: Unknown property %s", ontology_file, subject);
				return;
			}

			tracker_property_set_is_inverse_functional_property (property, TRUE);
		} else if (g_strcmp0 (object, TRACKER_PREFIX "Namespace") == 0) {
			TrackerNamespace *namespace;

			if (tracker_ontology_get_namespace_by_uri (subject) != NULL) {
				g_critical ("%s: Duplicate definition of namespace %s", ontology_file, subject);
				return;
			}

			namespace = tracker_namespace_new ();
			tracker_namespace_set_uri (namespace, subject);
			tracker_ontology_add_namespace (namespace);
			g_object_unref (namespace);
		}
	} else if (g_strcmp0 (predicate, RDFS_SUB_CLASS_OF) == 0) {
		TrackerClass *class, *super_class;

		class = tracker_ontology_get_class_by_uri (subject);
		if (class == NULL) {
			g_critical ("%s: Unknown class %s", ontology_file, subject);
			return;
		}

		super_class = tracker_ontology_get_class_by_uri (object);
		if (super_class == NULL) {
			g_critical ("%s: Unknown class %s", ontology_file, object);
			return;
		}

		tracker_class_add_super_class (class, super_class);
	} else if (g_strcmp0 (predicate, RDFS_SUB_PROPERTY_OF) == 0) {
		TrackerProperty *property, *super_property;

		property = tracker_ontology_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_file, subject);
			return;
		}

		super_property = tracker_ontology_get_property_by_uri (object);
		if (super_property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_file, object);
			return;
		}

		tracker_property_add_super_property (property, super_property);
	} else if (g_strcmp0 (predicate, RDFS_DOMAIN) == 0) {
		TrackerProperty *property;
		TrackerClass *domain;

		property = tracker_ontology_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_file, subject);
			return;
		}

		domain = tracker_ontology_get_class_by_uri (object);
		if (domain == NULL) {
			g_critical ("%s: Unknown class %s", ontology_file, object);
			return;
		}

		tracker_property_set_domain (property, domain);
	} else if (g_strcmp0 (predicate, RDFS_RANGE) == 0) {
		TrackerProperty *property;
		TrackerClass *range;

		property = tracker_ontology_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_file, subject);
			return;
		}

		range = tracker_ontology_get_class_by_uri (object);
		if (range == NULL) {
			g_critical ("%s: Unknown class %s", ontology_file, object);
			return;
		}

		tracker_property_set_range (property, range);
	} else if (g_strcmp0 (predicate, NRL_MAX_CARDINALITY) == 0) {
		TrackerProperty *property;

		property = tracker_ontology_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_file, subject);
			return;
		}

		if (atoi (object) == 1) {
			tracker_property_set_multiple_values (property, FALSE);
		}
	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "indexed") == 0) {
		TrackerProperty *property;

		property = tracker_ontology_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_file, subject);
			return;
		}

		if (strcmp (object, "true") == 0) {
			tracker_property_set_indexed (property, TRUE);
		}
	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "transient") == 0) {
		TrackerProperty *property;

		property = tracker_ontology_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_file, subject);
			return;
		}

		if (g_strcmp0 (object, "true") == 0) {
			tracker_property_set_transient (property, TRUE);
		}
	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "isAnnotation") == 0) {
		TrackerProperty *property;

		property = tracker_ontology_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_file, subject);
			return;
		}

		if (g_strcmp0 (object, "true") == 0) {
			tracker_property_set_embedded (property, FALSE);
		}
	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "fulltextIndexed") == 0) {
		TrackerProperty *property;

		property = tracker_ontology_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_file, subject);
			return;
		}

		if (strcmp (object, "true") == 0) {
			tracker_property_set_fulltext_indexed (property, TRUE);
		}
	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "fulltextNoLimit") == 0) {
		TrackerProperty *property;

		property = tracker_ontology_get_property_by_uri (subject);
		if (property == NULL) {
			g_critical ("%s: Unknown property %s", ontology_file, subject);
			return;
		}

		if (strcmp (object, "true") == 0) {
			tracker_property_set_fulltext_no_limit (property, TRUE);
		}
	} else if (g_strcmp0 (predicate, TRACKER_PREFIX "prefix") == 0) {
		TrackerNamespace *namespace;

		namespace = tracker_ontology_get_namespace_by_uri (subject);
		if (namespace == NULL) {
			g_critical ("%s: Unknown namespace %s", ontology_file, subject);
			return;
		}

		tracker_namespace_set_prefix (namespace, object);
	}
}

static void
load_ontology_file_from_path (const gchar        *ontology_file,
                              gint               *max_id)
{
	TrackerTurtleReader *reader;
	GError              *error = NULL;

	reader = tracker_turtle_reader_new (ontology_file, &error);
	if (error) {
		g_critical ("Turtle parse error: %s", error->message);
		g_error_free (error);
		return;
	}

	while (error == NULL && tracker_turtle_reader_next (reader, &error)) {
		const gchar *subject, *predicate, *object;

		subject = tracker_turtle_reader_get_subject (reader);
		predicate = tracker_turtle_reader_get_predicate (reader);
		object = tracker_turtle_reader_get_object (reader);

		load_ontology_statement (ontology_file, 0, subject, predicate, object, max_id);
	}

	g_object_unref (reader);

	if (error) {
		g_critical ("Turtle parse error: %s", error->message);
		g_error_free (error);
	}
}

static void
load_ontology_file (const gchar               *filename,
                    gint                      *max_id)
{
	gchar           *ontology_file;

	ontology_file = g_build_filename (ontologies_dir, filename, NULL);
	load_ontology_file_from_path (ontology_file, max_id);
	g_free (ontology_file);
}

static GHashTable *
load_ontology_from_journal (void)
{
	GHashTable *id_uri_map;

	id_uri_map = g_hash_table_new (g_direct_hash, g_direct_equal);

	while (tracker_db_journal_reader_next (NULL)) {
		TrackerDBJournalEntryType type;

		type = tracker_db_journal_reader_get_type ();
		if (type == TRACKER_DB_JOURNAL_RESOURCE) {
			gint id;
			const gchar *uri;

			tracker_db_journal_reader_get_resource (&id, &uri);
			g_hash_table_insert (id_uri_map, GINT_TO_POINTER (id), (gpointer) uri);
		} else if (type == TRACKER_DB_JOURNAL_END_TRANSACTION) {
			/* end of initial transaction => end of ontology */
			break;
		} else {
			const gchar *subject, *predicate, *object;
			gint subject_id, predicate_id, object_id;

			if (type == TRACKER_DB_JOURNAL_INSERT_STATEMENT) {
				tracker_db_journal_reader_get_statement (NULL, &subject_id, &predicate_id, &object);
			} else if (type == TRACKER_DB_JOURNAL_INSERT_STATEMENT_ID) {
				tracker_db_journal_reader_get_statement_id (NULL, &subject_id, &predicate_id, &object_id);
				object = g_hash_table_lookup (id_uri_map, GINT_TO_POINTER (object_id));
			} else {
				continue;
			}

			subject = g_hash_table_lookup (id_uri_map, GINT_TO_POINTER (subject_id));
			predicate = g_hash_table_lookup (id_uri_map, GINT_TO_POINTER (predicate_id));

			load_ontology_statement ("journal", subject_id, subject, predicate, object, NULL);
		}
	}

	g_hash_table_unref (id_uri_map);

	return id_uri_map;
}

static void
import_ontology_file (const gchar             *filename)
{
	gchar           *ontology_file;
	GError          *error = NULL;

	ontology_file = g_build_filename (ontologies_dir, filename, NULL);
	tracker_turtle_reader_load (ontology_file, &error);
	g_free (ontology_file);

	if (error) {
		g_critical ("%s", error->message);
		g_error_free (error);
	}
}

static gchar *
query_resource_by_id (gint id)
{
	TrackerDBCursor *cursor;
	TrackerDBInterface *iface;
	TrackerDBStatement *stmt;
	gchar *uri;

	g_return_val_if_fail (id > 0, NULL);

	iface = tracker_db_manager_get_db_interface ();

	stmt = tracker_db_interface_create_statement (iface,
	                                              "SELECT Uri FROM Resource WHERE ID = ?");
	tracker_db_statement_bind_int (stmt, 0, id);
	cursor = tracker_db_statement_start_cursor (stmt, NULL);
	g_object_unref (stmt);

	tracker_db_cursor_iter_next (cursor);
	uri = g_strdup (tracker_db_cursor_get_string (cursor, 0));
	g_object_unref (cursor);

	return uri;
}

static void
replay_journal (void)
{
	GError *journal_error = NULL;

	tracker_db_journal_reader_init (NULL);

	while (tracker_db_journal_reader_next (&journal_error)) {
		GError *error = NULL;
		TrackerDBJournalEntryType type;
		const gchar *graph, *subject, *predicate, *object;
		gint graph_id, subject_id, predicate_id, object_id;

		type = tracker_db_journal_reader_get_type ();
		if (type == TRACKER_DB_JOURNAL_RESOURCE) {
			TrackerDBInterface *iface;
			TrackerDBStatement *stmt;
			gint id;
			const gchar *uri;

			tracker_db_journal_reader_get_resource (&id, &uri);

			iface = tracker_db_manager_get_db_interface ();

			stmt = tracker_db_interface_create_statement (iface,
					                              "INSERT "
					                              "INTO Resource "
					                              "(ID, Uri) "
					                              "VALUES (?, ?)");
			tracker_db_statement_bind_int (stmt, 0, id);
			tracker_db_statement_bind_text (stmt, 1, uri);
			tracker_db_statement_execute (stmt, &error);
		} else if (type == TRACKER_DB_JOURNAL_START_TRANSACTION) {
			tracker_data_begin_replay_transaction (tracker_db_journal_reader_get_time ());
		} else if (type == TRACKER_DB_JOURNAL_END_TRANSACTION) {
			tracker_data_commit_transaction ();
		} else if (type == TRACKER_DB_JOURNAL_INSERT_STATEMENT) {
			tracker_db_journal_reader_get_statement (&graph_id, &subject_id, &predicate_id, &object);

			if (graph_id > 0) {
				graph = query_resource_by_id (graph_id);
			} else {
				graph = NULL;
			}
			subject = query_resource_by_id (subject_id);
			predicate = query_resource_by_id (predicate_id);

			tracker_data_insert_statement_with_string (graph, subject, predicate, object, &error);
		} else if (type == TRACKER_DB_JOURNAL_INSERT_STATEMENT_ID) {
			tracker_db_journal_reader_get_statement_id (&graph_id, &subject_id, &predicate_id, &object_id);

			if (graph_id > 0) {
				graph = query_resource_by_id (graph_id);
			} else {
				graph = NULL;
			}
			subject = query_resource_by_id (subject_id);
			predicate = query_resource_by_id (predicate_id);
			object = query_resource_by_id (object_id);

			tracker_data_insert_statement_with_uri (graph, subject, predicate, object, &error);
		} else if (type == TRACKER_DB_JOURNAL_DELETE_STATEMENT) {
			tracker_db_journal_reader_get_statement (&graph_id, &subject_id, &predicate_id, &object);

			if (graph_id > 0) {
				graph = query_resource_by_id (graph_id);
			} else {
				graph = NULL;
			}
			subject = query_resource_by_id (subject_id);
			predicate = query_resource_by_id (predicate_id);

			tracker_data_delete_statement (graph, subject, predicate, object, &error);
		} else if (type == TRACKER_DB_JOURNAL_DELETE_STATEMENT_ID) {
			tracker_db_journal_reader_get_statement_id (&graph_id, &subject_id, &predicate_id, &object_id);

			if (graph_id > 0) {
				graph = query_resource_by_id (graph_id);
			} else {
				graph = NULL;
			}
			subject = query_resource_by_id (subject_id);
			predicate = query_resource_by_id (predicate_id);
			object = query_resource_by_id (object_id);

			tracker_data_delete_statement (graph, subject, predicate, object, &error);
		}
	}


	if (journal_error) {
		gsize size;

		size = tracker_db_journal_reader_get_size_of_correct ();
		tracker_db_journal_reader_shutdown ();

		tracker_db_journal_init (NULL);
		tracker_db_journal_truncate (size);
		tracker_db_journal_shutdown ();

		g_clear_error (&journal_error);
	} else {
		tracker_db_journal_reader_shutdown ();
	}
}

static void
class_add_super_classes_from_db (TrackerDBInterface *iface, TrackerClass *class)
{
	TrackerDBStatement *stmt;
	TrackerDBCursor *cursor;

	stmt = tracker_db_interface_create_statement (iface,
	                                              "SELECT (SELECT Uri FROM Resource WHERE ID = \"rdfs:subClassOf\") "
	                                              "FROM \"rdfs:Class_rdfs:subClassOf\" "
	                                              "WHERE ID = (SELECT ID FROM Resource WHERE Uri = ?)");
	tracker_db_statement_bind_text (stmt, 0, tracker_class_get_uri (class));
	cursor = tracker_db_statement_start_cursor (stmt, NULL);
	g_object_unref (stmt);

	if (cursor) {
		while (tracker_db_cursor_iter_next (cursor)) {
			TrackerClass *super_class;
			const gchar *super_class_uri;

			super_class_uri = tracker_db_cursor_get_string (cursor, 0);
			super_class = tracker_ontology_get_class_by_uri (super_class_uri);
			tracker_class_add_super_class (class, super_class);
		}

		g_object_unref (cursor);
	}
}

static void
property_add_super_properties_from_db (TrackerDBInterface *iface, TrackerProperty *property)
{
	TrackerDBStatement *stmt;
	TrackerDBCursor *cursor;

	stmt = tracker_db_interface_create_statement (iface,
	                                              "SELECT (SELECT Uri FROM Resource WHERE ID = \"rdfs:subPropertyOf\") "
	                                              "FROM \"rdf:Property_rdfs:subPropertyOf\" "
	                                              "WHERE ID = (SELECT ID FROM Resource WHERE Uri = ?)");
	tracker_db_statement_bind_text (stmt, 0, tracker_property_get_uri (property));
	cursor = tracker_db_statement_start_cursor (stmt, NULL);
	g_object_unref (stmt);

	if (cursor) {
		while (tracker_db_cursor_iter_next (cursor)) {
			TrackerProperty *super_property;
			const gchar *super_property_uri;

			super_property_uri = tracker_db_cursor_get_string (cursor, 0);
			super_property = tracker_ontology_get_property_by_uri (super_property_uri);
			tracker_property_add_super_property (property, super_property);
		}

		g_object_unref (cursor);
	}
}

static void
db_get_static_data (TrackerDBInterface *iface)
{
	TrackerDBStatement *stmt;
	TrackerDBCursor *cursor;
	TrackerDBResultSet *result_set;

	stmt = tracker_db_interface_create_statement (iface,
	                                              "SELECT (SELECT Uri FROM Resource WHERE ID = \"tracker:Namespace\".ID), "
	                                              "\"tracker:prefix\" "
	                                              "FROM \"tracker:Namespace\"");
	cursor = tracker_db_statement_start_cursor (stmt, NULL);
	g_object_unref (stmt);

	if (cursor) {
		while (tracker_db_cursor_iter_next (cursor)) {
			TrackerNamespace *namespace;
			const gchar      *uri, *prefix;

			namespace = tracker_namespace_new ();

			uri = tracker_db_cursor_get_string (cursor, 0);
			prefix = tracker_db_cursor_get_string (cursor, 1);

			tracker_namespace_set_uri (namespace, uri);
			tracker_namespace_set_prefix (namespace, prefix);
			tracker_ontology_add_namespace (namespace);

			g_object_unref (namespace);

		}

		g_object_unref (cursor);
	}

	stmt = tracker_db_interface_create_statement (iface,
	                                              "SELECT \"rdfs:Class\".ID, (SELECT Uri FROM Resource WHERE ID = \"rdfs:Class\".ID) "
	                                              "FROM \"rdfs:Class\" ORDER BY ID");
	cursor = tracker_db_statement_start_cursor (stmt, NULL);
	g_object_unref (stmt);

	if (cursor) {
		while (tracker_db_cursor_iter_next (cursor)) {
			TrackerClass *class;
			const gchar  *uri;
			gint          id;
			gint          count;

			class = tracker_class_new ();

			id = tracker_db_cursor_get_int (cursor, 0);
			uri = tracker_db_cursor_get_string (cursor, 1);

			tracker_class_set_uri (class, uri);
			class_add_super_classes_from_db (iface, class);

			tracker_ontology_add_class (class);
			tracker_ontology_add_id_uri_pair (id, uri);
			tracker_class_set_id (class, id);

			/* xsd classes do not derive from rdfs:Resource and do not use separate tables */
			if (!g_str_has_prefix (tracker_class_get_name (class), "xsd:")) {
				/* update statistics */
				stmt = tracker_db_interface_create_statement (iface, "SELECT COUNT(1) FROM \"%s\"", tracker_class_get_name (class));
				result_set = tracker_db_statement_execute (stmt, NULL);
				tracker_db_result_set_get (result_set, 0, &count, -1);
				tracker_class_set_count (class, count);
				g_object_unref (result_set);
				g_object_unref (stmt);
			}

			g_object_unref (class);
		}

		g_object_unref (cursor);
	}

	stmt = tracker_db_interface_create_statement (iface,
	                                              "SELECT \"rdf:Property\".ID, (SELECT Uri FROM Resource WHERE ID = \"rdf:Property\".ID), "
	                                              "(SELECT Uri FROM Resource WHERE ID = \"rdfs:domain\"), "
	                                              "(SELECT Uri FROM Resource WHERE ID = \"rdfs:range\"), "
	                                              "\"nrl:maxCardinality\", "
	                                              "\"tracker:indexed\", "
	                                              "\"tracker:fulltextIndexed\", "
	                                              "\"tracker:fulltextNoLimit\", "
	                                              "\"tracker:transient\", "
	                                              "\"tracker:isAnnotation\", "
	                                              "(SELECT 1 FROM \"rdfs:Resource_rdf:type\" WHERE ID = \"rdf:Property\".ID AND "
	                                              "\"rdf:type\" = (SELECT ID FROM Resource WHERE Uri = '" NRL_INVERSE_FUNCTIONAL_PROPERTY "')) "
	                                              "FROM \"rdf:Property\" ORDER BY ID");
	cursor = tracker_db_statement_start_cursor (stmt, NULL);
	g_object_unref (stmt);

	if (cursor) {

		while (tracker_db_cursor_iter_next (cursor)) {
			GValue value = { 0 };
			TrackerProperty *property;
			const gchar     *uri, *domain_uri, *range_uri;
			gboolean         multi_valued, indexed, fulltext_indexed, fulltext_no_limit;
			gboolean         transient, annotation, is_inverse_functional_property;
			gint             id;

			property = tracker_property_new ();

			id = tracker_db_cursor_get_int (cursor, 0);
			uri = tracker_db_cursor_get_string (cursor, 1);
			domain_uri = tracker_db_cursor_get_string (cursor, 2);
			range_uri = tracker_db_cursor_get_string (cursor, 3);

			tracker_db_cursor_get_value (cursor, 4, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				multi_valued = (g_value_get_int (&value) > 1);
				g_value_unset (&value);
			} else {
				/* nrl:maxCardinality not set
				   not limited to single value */
				multi_valued = TRUE;
			}

			tracker_db_cursor_get_value (cursor, 5, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				indexed = (g_value_get_int (&value) == 1);
				g_value_unset (&value);
			} else {
				/* NULL */
				indexed = FALSE;
			}

			tracker_db_cursor_get_value (cursor, 6, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				fulltext_indexed = (g_value_get_int (&value) == 1);
				g_value_unset (&value);
			} else {
				/* NULL */
				fulltext_indexed = FALSE;
			}

			tracker_db_cursor_get_value (cursor, 7, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				fulltext_no_limit = (g_value_get_int (&value) == 1);
				g_value_unset (&value);
			} else {
				/* NULL */
				fulltext_no_limit = FALSE;
			}

			tracker_db_cursor_get_value (cursor, 8, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				transient = (g_value_get_int (&value) == 1);
				g_value_unset (&value);
			} else {
				/* NULL */
				transient = FALSE;
			}

			tracker_db_cursor_get_value (cursor, 9, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				annotation = (g_value_get_int (&value) == 1);
				g_value_unset (&value);
			} else {
				/* NULL */
				annotation = FALSE;
			}

			tracker_db_cursor_get_value (cursor, 10, &value);

			if (G_VALUE_TYPE (&value) != 0) {
				is_inverse_functional_property = TRUE;
				g_value_unset (&value);
			} else {
				/* NULL */
				is_inverse_functional_property = FALSE;
			}

			tracker_property_set_transient (property, transient);
			tracker_property_set_uri (property, uri);
			tracker_property_set_id (property, id);
			tracker_property_set_domain (property, tracker_ontology_get_class_by_uri (domain_uri));
			tracker_property_set_range (property, tracker_ontology_get_class_by_uri (range_uri));
			tracker_property_set_multiple_values (property, multi_valued);
			tracker_property_set_indexed (property, indexed);
			tracker_property_set_fulltext_indexed (property, fulltext_indexed);
			tracker_property_set_fulltext_no_limit (property, fulltext_no_limit);
			tracker_property_set_embedded (property, !annotation);
			tracker_property_set_is_inverse_functional_property (property, is_inverse_functional_property);
			property_add_super_properties_from_db (iface, property);

			tracker_ontology_add_property (property);
			tracker_ontology_add_id_uri_pair (id, uri);

			g_object_unref (property);

		}

		g_object_unref (cursor);
	}
}


static void
insert_uri_in_resource_table (TrackerDBInterface *iface,
                              const gchar        *uri,
                              gint                id)
{
	TrackerDBStatement *stmt;
	GError *error = NULL;

	stmt = tracker_db_interface_create_statement (iface,
	                                              "INSERT "
	                                              "INTO Resource "
	                                              "(ID, Uri) "
	                                              "VALUES (?, ?)");
	tracker_db_statement_bind_int (stmt, 0, id);
	tracker_db_statement_bind_text (stmt, 1, uri);
	tracker_db_statement_execute (stmt, &error);

	if (error) {
		g_critical ("%s\n", error->message);
		g_clear_error (&error);
	}

	if (!in_journal_replay) {
		tracker_db_journal_append_resource (id, uri);
	}

	g_object_unref (stmt);

}

static void
create_decomposed_metadata_property_table (TrackerDBInterface *iface,
                                           TrackerProperty    *property,
                                           const gchar        *service_name,
                                           const gchar       **sql_type_for_single_value)
{
	const char *field_name;
	const char *sql_type;
	gboolean    transient;

	field_name = tracker_property_get_name (property);

	transient = !sql_type_for_single_value;

	if (!transient) {
		transient = tracker_property_get_transient (property);
	}

	switch (tracker_property_get_data_type (property)) {
	case TRACKER_PROPERTY_TYPE_STRING:
		sql_type = "TEXT";
		break;
	case TRACKER_PROPERTY_TYPE_INTEGER:
	case TRACKER_PROPERTY_TYPE_BOOLEAN:
	case TRACKER_PROPERTY_TYPE_DATE:
	case TRACKER_PROPERTY_TYPE_DATETIME:
	case TRACKER_PROPERTY_TYPE_RESOURCE:
		sql_type = "INTEGER";
		break;
	case TRACKER_PROPERTY_TYPE_DOUBLE:
		sql_type = "REAL";
		break;
	default:
		sql_type = "";
		break;
	}

	if (transient || tracker_property_get_multiple_values (property)) {
		GString *sql;

		sql = g_string_new ("");
		g_string_append_printf (sql, "CREATE %sTABLE \"%s_%s\" ("
		                             "ID INTEGER NOT NULL, "
		                             "\"%s\" %s NOT NULL, "
		                             "\"%s:graph\" INTEGER",
		                             transient ? "TEMPORARY " : "",
		                             service_name,
		                             field_name,
		                             field_name,
		                             sql_type,
		                             field_name);

		if (tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_DATETIME) {
			/* xsd:dateTime is stored in three columns:
			 * universal time, local date, local time of day */
			g_string_append_printf (sql,
			                        ", \"%s:localDate\" INTEGER NOT NULL"
			                        ", \"%s:localTime\" INTEGER NOT NULL",
			                        tracker_property_get_name (property),
			                        tracker_property_get_name (property));
		}

		/* multiple values */
		if (tracker_property_get_indexed (property)) {
			/* use different UNIQUE index for properties whose
			 * value should be indexed to minimize index size */
			tracker_db_interface_execute_query (iface, NULL,
			                                    "%s, "
			                                    "UNIQUE (\"%s\", ID))",
			                                    sql->str,
			                                    field_name);

			tracker_db_interface_execute_query (iface, NULL,
			                                    "CREATE INDEX \"%s_%s_ID\" ON \"%s_%s\" (ID)",
			                                    service_name,
			                                    field_name,
			                                    service_name,
			                                    field_name);
		} else {
			/* we still have to include the property value in
			 * the unique index for proper constraints */
			tracker_db_interface_execute_query (iface, NULL,
			                                    "%s, "
			                                    "UNIQUE (ID, \"%s\"))",
			                                    sql->str,
			                                    field_name);
		}

		g_string_free (sql, TRUE);
	} else if (sql_type_for_single_value) {
		*sql_type_for_single_value = sql_type;
	}
}

static void
create_decomposed_metadata_tables (TrackerDBInterface *iface,
                                   TrackerClass       *service)
{
	const char *service_name;
	GString    *sql;
	TrackerProperty           **properties, *property;
	GSList      *class_properties, *field_it;
	gboolean    main_class;
	gint        i, n_props;

	service_name = tracker_class_get_name (service);
	main_class = (strcmp (service_name, "rdfs:Resource") == 0);

	if (g_str_has_prefix (service_name, "xsd:")) {
		/* xsd classes do not derive from rdfs:Resource and do not need separate tables */
		return;
	}

	sql = g_string_new ("");
	g_string_append_printf (sql, "CREATE TABLE \"%s\" (ID INTEGER NOT NULL PRIMARY KEY", service_name);
	if (main_class) {
		tracker_db_interface_execute_query (iface, NULL, "CREATE TABLE Resource (ID INTEGER NOT NULL PRIMARY KEY, Uri Text NOT NULL, UNIQUE (Uri))");

		g_string_append (sql, ", Available INTEGER NOT NULL");
	}

	properties = tracker_ontology_get_properties (&n_props);
	class_properties = NULL;

	for (i = 0; i < n_props; i++) {
		property = properties[i];

		if (tracker_property_get_domain (property) == service) {
			const gchar *sql_type_for_single_value = NULL;

			create_decomposed_metadata_property_table (iface, property,
			                                           service_name,
			                                           &sql_type_for_single_value);

			if (sql_type_for_single_value) {
				/* single value */

				class_properties = g_slist_prepend (class_properties, property);

				g_string_append_printf (sql, ", \"%s\" %s",
				                        tracker_property_get_name (property),
				                        sql_type_for_single_value);
				if (tracker_property_get_is_inverse_functional_property (property)) {
					g_string_append (sql, " UNIQUE");
				}

				g_string_append_printf (sql, ", \"%s:graph\" INTEGER",
				                        tracker_property_get_name (property));

				if (tracker_property_get_data_type (property) == TRACKER_PROPERTY_TYPE_DATETIME) {
					/* xsd:dateTime is stored in three columns:
					 * universal time, local date, local time of day */
					g_string_append_printf (sql, ", \"%s:localDate\" INTEGER, \"%s:localTime\" INTEGER",
						                tracker_property_get_name (property),
						                tracker_property_get_name (property));
				}
			}
		}
	}

	g_string_append (sql, ")");
	tracker_db_interface_execute_query (iface, NULL, "%s", sql->str);

	g_string_free (sql, TRUE);

	/* create index for single-valued fields */
	for (field_it = class_properties; field_it != NULL; field_it = field_it->next) {
		TrackerProperty *field;
		const char   *field_name;

		field = field_it->data;

		if (!tracker_property_get_multiple_values (field)
		    && tracker_property_get_indexed (field)) {
			field_name = tracker_property_get_name (field);
			tracker_db_interface_execute_query (iface, NULL,
			                                    "CREATE INDEX \"%s_%s\" ON \"%s\" (\"%s\")",
			                                    service_name,
			                                    field_name,
			                                    service_name,
			                                    field_name);
		}
	}

	g_slist_free (class_properties);

}

static void
create_decomposed_transient_metadata_tables (TrackerDBInterface *iface)
{
	TrackerProperty **properties;
	TrackerProperty *property;
	gint i, n_props;

	properties = tracker_ontology_get_properties (&n_props);

	for (i = 0; i < n_props; i++) {
		property = properties[i];

		if (tracker_property_get_transient (property)) {

			TrackerClass *domain;
			const gchar *service_name;
			const char *field_name;

			field_name = tracker_property_get_name (property);

			domain = tracker_property_get_domain (property);
			service_name = tracker_class_get_name (domain);

			/* create the TEMPORARY table */
			create_decomposed_metadata_property_table (iface, property,
			                                           service_name,
			                                           NULL);
		}
	}
}

static void
create_fts_table (TrackerDBInterface *iface)
{
	gchar *query = tracker_fts_get_create_fts_table_query ();

	tracker_db_interface_execute_query (iface, NULL, "%s", query);

	g_free (query);
}

static void
import_ontology_into_db (void)
{
	TrackerDBInterface *iface;

	TrackerClass **classes;
	TrackerProperty **properties;
	gint i, n_props, n_classes;

	iface = tracker_db_manager_get_db_interface ();

	classes = tracker_ontology_get_classes (&n_classes);
	properties = tracker_ontology_get_properties (&n_props);

	/* create tables */
	for (i = 0; i < n_classes; i++) {
		create_decomposed_metadata_tables (iface, classes[i]);
	}

	create_fts_table (iface);

	/* insert classes into rdfs:Resource table */
	for (i = 0; i < n_classes; i++) {
		insert_uri_in_resource_table (iface, tracker_class_get_uri (classes[i]),
		                              tracker_class_get_id (classes[i]));
	}

	/* insert properties into rdfs:Resource table */
	for (i = 0; i < n_props; i++) {
		insert_uri_in_resource_table (iface, tracker_property_get_uri (properties[i]),
		                              tracker_property_get_id (properties[i]));
	}
}

gboolean
tracker_data_manager_init (TrackerDBManagerFlags  flags,
                           const gchar           *test_schema,
                           gboolean              *first_time)
{
	TrackerDBInterface *iface;
	gboolean is_first_time_index, read_journal;

	/* First set defaults for return values */
	if (first_time) {
		*first_time = FALSE;
	}

	if (initialized) {
		return TRUE;
	}

	read_journal = FALSE;

	tracker_db_manager_init (flags, &is_first_time_index, FALSE);

	if (first_time != NULL) {
		*first_time = is_first_time_index;
	}

	iface = tracker_db_manager_get_db_interface ();

	if (is_first_time_index && !test_schema) {
		if (tracker_db_journal_reader_init (NULL)) {
			if (tracker_db_journal_reader_next (NULL)) {
				/* journal with at least one valid transaction
				   is required to trigger journal replay */
				read_journal = TRUE;
			} else {
				tracker_db_journal_reader_shutdown ();
			}
		}
	}

	if (read_journal) {
		in_journal_replay = TRUE;

		/* load ontology from journal into memory */
		load_ontology_from_journal ();

		tracker_data_begin_replay_transaction (tracker_db_journal_reader_get_time ());
		import_ontology_into_db ();
		tracker_data_commit_transaction ();

		tracker_db_journal_reader_shutdown ();

		replay_journal ();

		in_journal_replay = FALSE;

		/* open journal for writing */
		tracker_db_journal_init (NULL);
	} else if (is_first_time_index) {
		gint max_id = 0;
		GList *sorted = NULL, *l;
		gchar *test_schema_path = NULL;
		const gchar *env_path;
		GError *error = NULL;

		env_path = g_getenv ("TRACKER_DB_ONTOLOGIES_DIR");

		if (G_LIKELY (!env_path)) {
			ontologies_dir = g_build_filename (SHAREDIR,
			                                   "tracker",
			                                   "ontologies",
			                                   NULL);
		} else {
			ontologies_dir = g_strdup (env_path);
		}

		if (test_schema) {
			/* load test schema, not used in normal operation */
			test_schema_path = g_strconcat (test_schema, ".ontology", NULL);

			sorted = g_list_prepend (sorted, g_strdup ("12-nrl.ontology"));
			sorted = g_list_prepend (sorted, g_strdup ("11-rdf.ontology"));
			sorted = g_list_prepend (sorted, g_strdup ("10-xsd.ontology"));
		} else {
			GDir        *ontologies;
			const gchar *conf_file;

			ontologies = g_dir_open (ontologies_dir, 0, NULL);

			conf_file = g_dir_read_name (ontologies);

			/* .ontology files */
			while (conf_file) {
				if (g_str_has_suffix (conf_file, ".ontology")) {
					sorted = g_list_insert_sorted (sorted,
					                               g_strdup (conf_file),
					                               (GCompareFunc) strcmp);
				}
				conf_file = g_dir_read_name (ontologies);
			}

			g_dir_close (ontologies);
		}

		tracker_db_journal_init (NULL);

		/* load ontology from files into memory */
		for (l = sorted; l; l = l->next) {
			g_debug ("Loading ontology %s", (char *) l->data);
			load_ontology_file (l->data, &max_id);
		}

		if (test_schema) {
			g_debug ("Loading ontology:'%s' (TEST ONTOLOGY)", test_schema_path);

			load_ontology_file_from_path (test_schema_path, &max_id);
		}

		tracker_data_begin_transaction ();
		tracker_db_journal_start_transaction (time (NULL));

		import_ontology_into_db ();

		/* store ontology in database */
		for (l = sorted; l; l = l->next) {
			import_ontology_file (l->data);
		}
		if (test_schema) {
			tracker_turtle_reader_load (test_schema_path, &error);
			g_free (test_schema_path);

			if (error) {
				g_critical ("%s", error->message);
				g_error_free (error);
			}
		}

		tracker_db_journal_commit_transaction ();
		tracker_data_commit_transaction ();

		g_list_foreach (sorted, (GFunc) g_free, NULL);
		g_list_free (sorted);

		g_free (ontologies_dir);
		ontologies_dir = NULL;
	} else {
		tracker_db_journal_init (NULL);

		/* load ontology from database into memory */
		db_get_static_data (iface);
		create_decomposed_transient_metadata_tables (iface);
	}

	/* ensure FTS is fully initialized */
	tracker_db_interface_execute_query (iface, NULL, "SELECT 1 FROM fulltext.fts WHERE rowid = 0");

	initialized = TRUE;

	return TRUE;
}


void
tracker_data_manager_shutdown (void)
{
	g_return_if_fail (initialized == TRUE);

	tracker_db_journal_shutdown ();
	tracker_db_manager_shutdown ();

	initialized = FALSE;
}

gint64
tracker_data_manager_get_db_option_int64 (const gchar *option)
{
	TrackerDBInterface *iface;
	TrackerDBStatement *stmt;
	TrackerDBResultSet *result_set;
	gchar              *str;
	gint                value = 0;

	g_return_val_if_fail (option != NULL, 0);

	iface = tracker_db_manager_get_db_interface ();

	stmt = tracker_db_interface_create_statement (iface, "SELECT OptionValue FROM Options WHERE OptionKey = ?");
	tracker_db_statement_bind_text (stmt, 0, option);
	result_set = tracker_db_statement_execute (stmt, NULL);
	g_object_unref (stmt);

	if (result_set) {
		tracker_db_result_set_get (result_set, 0, &str, -1);

		if (str) {
			value = g_ascii_strtoull (str, NULL, 10);
			g_free (str);
		}

		g_object_unref (result_set);
	}

	return value;
}

void
tracker_data_manager_set_db_option_int64 (const gchar *option,
                                          gint64       value)
{
	TrackerDBInterface *iface;
	TrackerDBStatement *stmt;
	gchar              *str;

	g_return_if_fail (option != NULL);

	iface = tracker_db_manager_get_db_interface ();

	stmt = tracker_db_interface_create_statement (iface, "REPLACE INTO Options (OptionKey, OptionValue) VALUES (?,?)");
	tracker_db_statement_bind_text (stmt, 0, option);

	str = g_strdup_printf ("%"G_GINT64_FORMAT, value);
	tracker_db_statement_bind_text (stmt, 1, str);
	g_free (str);

	tracker_db_statement_execute (stmt, NULL);
	g_object_unref (stmt);
}

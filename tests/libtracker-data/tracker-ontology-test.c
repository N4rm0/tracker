/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2009, Nokia (urho.konttori@nokia.com)
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

#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#include <raptor.h>

#include <libtracker-db/tracker-db-manager.h>

#include <libtracker-data/tracker-data-manager.h>
#include <libtracker-data/tracker-data-query.h>
#include <libtracker-data/tracker-data-update.h>
#include <libtracker-data/tracker-turtle.h>

typedef struct _TestInfo TestInfo;

struct _TestInfo {
	const gchar *test_name;
	const gchar *data;
};

const TestInfo nie_tests[] = {
	{ "nie/filter-subject-1", "nie/data-1" },
	{ "nie/filter-characterset-1", "nie/data-1" },
	{ "nie/filter-comment-1", "nie/data-1" },
	{ "nie/filter-description-1", "nie/data-1" },
	{ "nie/filter-generator-1", "nie/data-1" },
	{ "nie/filter-identifier-1", "nie/data-1" },
	{ "nie/filter-keyword-1", "nie/data-1" },
	{ "nie/filter-language-1", "nie/data-1" },
	{ "nie/filter-legal-1", "nie/data-1" },
	{ "nie/filter-title-1", "nie/data-1" },
	{ "nie/filter-version-1", "nie/data-1" },
	{ NULL }
};

const TestInfo nmo_tests[] = {
	{ "nmo/filter-charset-1", "nmo/data-1" },
	{ "nmo/filter-contentdescription-1", "nmo/data-1" },
	{ "nmo/filter-contentid-1", "nmo/data-1" },
	{ "nmo/filter-contenttransferencoding-1", "nmo/data-1" },
	{ "nmo/filter-headername-1", "nmo/data-1" },
	{ "nmo/filter-headervalue-1", "nmo/data-1" },
	{ "nmo/filter-isanswered-1", "nmo/data-1" },
	{ "nmo/filter-isdeleted-1", "nmo/data-1" },
	{ "nmo/filter-isdraft-1", "nmo/data-1" },
	{ "nmo/filter-isflagged-1", "nmo/data-1" },
	{ "nmo/filter-isread-1", "nmo/data-1" },
	{ "nmo/filter-isrecent-1", "nmo/data-1" },
	{ "nmo/filter-messageid-1", "nmo/data-1" },
	{ "nmo/filter-messagesubject-1", "nmo/data-1" }, 
	{ "nmo/filter-plaintextmessagecontent-1", "nmo/data-1" },
	{ NULL }
};

static void
consume_triple_storer (const gchar *subject,
                       const gchar *predicate,
                       const gchar *object,
                       void        *user_data)
{
	tracker_data_insert_statement (subject, predicate, object, NULL);
}

static void
test_query (gconstpointer test_data)
{
	GError      *error;
	gchar       *data_filename;
	gchar       *query, *query_filename;
	gchar       *results, *results_filename;
	TrackerConfig   *config;
	TrackerLanguage	*language;
	TrackerDBResultSet *result_set;
	const TestInfo *test_info;
	GString *test_results;

	error = NULL;
	test_info = test_data;

	/* initialization */

	config = tracker_config_new ();
	language = tracker_language_new (config);

	tracker_data_manager_init (config, language,
	                           TRACKER_DB_MANAGER_FORCE_REINDEX
	                           | TRACKER_DB_MANAGER_TEST_MODE,
	                           NULL, NULL);

	/* load data set */

	data_filename = g_strconcat (test_info->data, ".ttl", NULL);
	tracker_data_begin_transaction ();
	tracker_turtle_process (data_filename, NULL, consume_triple_storer, NULL);
	tracker_data_commit_transaction ();

	query_filename = g_strconcat (test_info->test_name, ".rq", NULL);
	g_file_get_contents (query_filename, &query, NULL, &error);
	g_assert (error == NULL);

	results_filename = g_strconcat (test_info->test_name, ".out", NULL);
	g_file_get_contents (results_filename, &results, NULL, &error);
	g_assert (error == NULL);

	/* perform actual query */

	result_set = tracker_data_query_sparql (query, &error);
	g_assert (error == NULL);

	/* compare results with reference output */

	test_results = g_string_new ("");

	if (result_set) {
		gboolean valid = TRUE;
		guint col_count;
		gint col;

		col_count = tracker_db_result_set_get_n_columns (result_set);

		while (valid) {
			for (col = 0; col < col_count; col++) {
				GValue value = { 0 };

				_tracker_db_result_set_get_value (result_set, col, &value);

				switch (G_VALUE_TYPE (&value)) {
				case G_TYPE_INT:
					g_string_append_printf (test_results, "\"%d\"", g_value_get_int (&value));
					break;
				case G_TYPE_DOUBLE:
					g_string_append_printf (test_results, "\"%f\"", g_value_get_double (&value));
					break;
				case G_TYPE_STRING:
					g_string_append_printf (test_results, "\"%s\"", g_value_get_string (&value));
					break;
				default:
					/* unbound variable */
					break;
				}

				if (col < col_count - 1) {
					g_string_append (test_results, "\t");
				}
			}

			g_string_append (test_results, "\n");

			valid = tracker_db_result_set_iter_next (result_set);
		}

		g_object_unref (result_set);
	}

	if (strcmp (results, test_results->str)) {
		/* print result difference */
		gchar *quoted_results;
		gchar *command_line;
		gchar *quoted_command_line;
		gchar *shell;
		gchar *diff;

		quoted_results = g_shell_quote (test_results->str);
		command_line = g_strdup_printf ("echo -n %s | diff -u %s -", quoted_results, results_filename);
		quoted_command_line = g_shell_quote (command_line);
		shell = g_strdup_printf ("sh -c %s", quoted_command_line);
		g_spawn_command_line_sync (shell, &diff, NULL, NULL, &error);
		g_assert (error == NULL);

		g_error ("%s", diff);

		g_free (quoted_results);
		g_free (command_line);
		g_free (quoted_command_line);
		g_free (shell);
		g_free (diff);
	}

	/* cleanup */

	g_free (data_filename);
	g_free (query_filename);
	g_free (query);
	g_free (results_filename);
	g_free (results);
	g_string_free (test_results, TRUE);

	tracker_data_manager_shutdown ();
}

int
main (int argc, char **argv)
{

	int              result;
	int              i;

	g_type_init ();
	g_thread_init (NULL);
	g_test_init (&argc, &argv, NULL);

	tracker_turtle_init ();

	/* add test cases */

	for (i = 0; nie_tests[i].test_name; i++) {
		gchar *testpath;
		g_message ("%d", i);
		
		testpath = g_strconcat ("/libtracker-data/nie/", nie_tests[i].test_name, NULL);
		g_test_add_data_func (testpath, &nie_tests[i], test_query);
		g_free (testpath);
	}

	for (i = 0; nmo_tests[i].test_name; i++) {
		gchar *testpath;
		g_message ("%d", i);
		
		testpath = g_strconcat ("/libtracker-data/nmo/", nmo_tests[i].test_name, NULL);
		g_test_add_data_func (testpath, &nmo_tests[i], test_query);
		g_free (testpath);
	}

	/* run tests */

	result = g_test_run ();

	tracker_turtle_shutdown ();

	return result;
}

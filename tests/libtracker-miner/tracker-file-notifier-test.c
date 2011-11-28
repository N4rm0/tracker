/*
 * Copyright (C) 2011, Nokia <ivan.frade@nokia.com>
 *
 * Author: Carlos Garnacho  <carlos@lanedo.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <libtracker-miner/tracker-miner-enums.h>
#include <libtracker-miner/tracker-file-notifier.h>

/* Fixture struct */
typedef struct {
	GFile *test_file;
	gchar *test_path;

	TrackerIndexingTree *indexing_tree;
	GMainLoop *main_loop;

	/* The file notifier to test */
	TrackerFileNotifier *notifier;

	guint expire_timeout_id;
	gboolean expect_finished;

	GList *ops;
} TestCommonContext;

typedef enum {
	OPERATION_CREATE,
	OPERATION_UPDATE,
	OPERATION_DELETE,
	OPERATION_MOVE
} OperationType;

typedef struct {
	gint op;
	gchar *path;
	gchar *other_path;
} FilesystemOperation;

#define test_add(path,fun)	  \
	g_test_add (path, \
	            TestCommonContext, \
	            NULL, \
	            test_common_context_setup, \
	            fun, \
	            test_common_context_teardown)

static void
filesystem_operation_free (FilesystemOperation *op)
{
	g_free (op->path);
	g_free (op->other_path);
	g_free (op);
}

static void
perform_file_operation (TestCommonContext *fixture,
                        gchar             *command,
                        gchar             *filename,
                        gchar             *other_filename)
{
	gchar *path, *other_path, *call;

	path = g_build_filename (fixture->test_path, filename, NULL);

	if (other_filename) {
		other_path = g_build_filename (fixture->test_path, filename, NULL);
		call = g_strdup_printf ("%s %s %s", command, path, other_path);
		g_free (other_path);
	} else {
		call = g_strdup_printf ("%s %s", command, path);
	}

	system (call);

	g_free (call);
	g_free (path);
}

#define CREATE_FOLDER(fixture,p) perform_file_operation((fixture),"mkdir",(p),NULL)
#define CREATE_UPDATE_FILE(fixture,p) perform_file_operation((fixture),"touch",(p),NULL)
#define DELETE_FILE(fixture,p) perform_file_operation((fixture),"rm",(p),NULL)
#define DELETE_FOLDER(fixture,p) perform_file_operation((fixture),"rm -rf",(p),NULL)

static void
file_notifier_file_created_cb (TrackerFileNotifier *notifier,
                               GFile               *file,
                               gpointer             user_data)
{
	TestCommonContext *fixture = user_data;
	FilesystemOperation *op;

	op = g_new0 (FilesystemOperation, 1);
	op->op = OPERATION_CREATE;
	op->path = g_file_get_relative_path (fixture->test_file , file);

	fixture->ops = g_list_prepend (fixture->ops, op);
}

static void
file_notifier_file_updated_cb (TrackerFileNotifier *notifier,
                               GFile               *file,
                               gboolean             attributes_only,
                               gpointer             user_data)
{
	TestCommonContext *fixture = user_data;
	FilesystemOperation *op;

	op = g_new0 (FilesystemOperation, 1);
	op->op = OPERATION_UPDATE;
	op->path = g_file_get_relative_path (fixture->test_file , file);

	fixture->ops = g_list_prepend (fixture->ops, op);
}

static void
file_notifier_file_deleted_cb (TrackerFileNotifier *notifier,
                               GFile               *file,
                               gpointer             user_data)
{
	TestCommonContext *fixture = user_data;
	FilesystemOperation *op;

	op = g_new0 (FilesystemOperation, 1);
	op->op = OPERATION_DELETE;
	op->path = g_file_get_relative_path (fixture->test_file , file);

	fixture->ops = g_list_prepend (fixture->ops, op);
}

static void
file_notifier_file_moved_cb (TrackerFileNotifier *notifier,
                             GFile               *file,
                             GFile               *other_file,
                             gpointer             user_data)
{
	TestCommonContext *fixture = user_data;
	FilesystemOperation *op;

	op = g_new0 (FilesystemOperation, 1);
	op->op = OPERATION_MOVE;
	op->path = g_file_get_relative_path (fixture->test_file , file);
	op->other_path = g_file_get_relative_path (fixture->test_file ,
						   other_file);

	fixture->ops = g_list_prepend (fixture->ops, op);
}

static void
file_notifier_finished_cb (TrackerFileNotifier *notifier,
			   gpointer             user_data)
{
	TestCommonContext *fixture = user_data;

	if (fixture->expect_finished) {
		g_main_loop_quit (fixture->main_loop);
	};
}

static void
test_common_context_index_dir (TestCommonContext     *fixture,
                               const gchar           *filename,
                               TrackerDirectoryFlags  flags)
{
	GFile *file;
	gchar *path;

	path = g_build_filename (fixture->test_path, filename, NULL);
	file = g_file_new_for_path (path);
	g_free (path);

	tracker_indexing_tree_add (fixture->indexing_tree, file, flags);
	g_object_unref (file);
}

static void
test_common_context_setup (TestCommonContext *fixture,
                           gconstpointer      data)
{
	fixture->test_path = g_build_filename (g_get_tmp_dir (),
	                                       "tracker-test-XXXXXX",
	                                       NULL);
	fixture->test_path = g_mkdtemp (fixture->test_path);
	fixture->test_file = g_file_new_for_path (fixture->test_path);

	fixture->ops = NULL;

	/* Create basic folders within the test location */
	CREATE_FOLDER (fixture, "recursive");
	CREATE_FOLDER (fixture, "non-recursive");
	CREATE_FOLDER (fixture, "non-indexed");

	fixture->indexing_tree = tracker_indexing_tree_new ();
	tracker_indexing_tree_set_filter_hidden (fixture->indexing_tree, TRUE);

	fixture->main_loop = g_main_loop_new (NULL, FALSE);
	fixture->notifier = tracker_file_notifier_new (fixture->indexing_tree);

	g_signal_connect (fixture->notifier, "file-created",
	                  G_CALLBACK (file_notifier_file_created_cb), fixture);
	g_signal_connect (fixture->notifier, "file-updated",
	                  G_CALLBACK (file_notifier_file_updated_cb), fixture);
	g_signal_connect (fixture->notifier, "file-deleted",
	                  G_CALLBACK (file_notifier_file_deleted_cb), fixture);
	g_signal_connect (fixture->notifier, "file-moved",
	                  G_CALLBACK (file_notifier_file_moved_cb), fixture);
	g_signal_connect (fixture->notifier, "finished",
	                  G_CALLBACK (file_notifier_finished_cb), fixture);
}

static void
test_common_context_teardown (TestCommonContext *fixture,
                              gconstpointer      data)
{
	DELETE_FOLDER (fixture, NULL);

	g_list_foreach (fixture->ops, (GFunc) filesystem_operation_free, NULL);
	g_list_free (fixture->ops);

	if (fixture->notifier) {
		g_object_unref (fixture->notifier);
	}

	if (fixture->indexing_tree) {
		g_object_unref (fixture->indexing_tree);
	}

	if (fixture->test_file) {
		g_object_unref (fixture->test_file);
	}

	if (fixture->test_path) {
		g_free (fixture->test_path);
	}

}

static gboolean
timeout_expired_cb (gpointer user_data)
{
	TestCommonContext *fixture = user_data;

	fixture->expire_timeout_id = 0;
	g_main_loop_quit (fixture->main_loop);

	return FALSE;
}

static void
test_common_context_expect_results (TestCommonContext   *fixture,
                                    FilesystemOperation *results,
                                    guint                n_results,
				    guint                max_timeout,
				    gboolean             expect_finished)
{
	GList *ops;
	guint i;

	fixture->expect_finished = expect_finished;

	if (max_timeout != 0) {
		g_timeout_add_seconds (max_timeout,
		                       (GSourceFunc) timeout_expired_cb,
		                       fixture);
	}

	g_main_loop_run (fixture->main_loop);

	g_assert_cmpint (n_results, ==, g_list_length (fixture->ops));

	for (i = 0; i < n_results; i++) {
		gboolean matched = FALSE;

		ops = fixture->ops;

		while (ops) {
			FilesystemOperation *op = ops->data;

			if (op->op == results[i].op &&
			    g_strcmp0 (op->path, results[i].path) == 0 &&
			    g_strcmp0 (op->other_path, results[i].other_path) == 0) {
				filesystem_operation_free (op);
				fixture->ops = g_list_delete_link (fixture->ops, ops);
				matched = TRUE;
				break;
			}

			ops = ops->next;
		}

		if (!matched) {
			if (results[i].op == OPERATION_MOVE) {
				g_critical ("Expected operation %d on %s (-> %s) didn't happen",
					    results[i].op, results[i].path,
					    results[i].other_path);
			} else {
				g_critical ("Expected operation %d on %s didn't happen",
					    results[i].op, results[i].path);
			}
		}
	}

	ops = fixture->ops;

	while (ops) {
		FilesystemOperation *op = ops->data;

		if (op->op == OPERATION_MOVE) {
			g_critical ("Unexpected operation %d on %s (-> %s) happened",
				    op->op, op->path,
				    op->other_path);
		} else {
			g_critical ("Unexpected operation %d on %s happened",
				    op->op, op->path);
		}
	}

	g_assert_cmpint (g_list_length (fixture->ops), ==, 0);
}

static void
test_file_notifier_crawling_non_recursive (TestCommonContext *fixture,
                                           gconstpointer      data)
{
	FilesystemOperation expected_results[] = {
		{ OPERATION_CREATE, "non-recursive", NULL },
		{ OPERATION_CREATE, "non-recursive/folder", NULL },
		{ OPERATION_CREATE, "non-recursive/bbb", NULL },
	};

	CREATE_FOLDER (fixture, "non-recursive/folder");
	CREATE_UPDATE_FILE (fixture, "non-recursive/folder/aaa");
	CREATE_UPDATE_FILE (fixture, "non-recursive/bbb");

	test_common_context_index_dir (fixture, "non-recursive",
	                               TRACKER_DIRECTORY_FLAG_NONE);

	tracker_file_notifier_start (fixture->notifier);

	test_common_context_expect_results (fixture, expected_results,
					    G_N_ELEMENTS (expected_results),
					    2, TRUE);

	tracker_file_notifier_stop (fixture->notifier);
}

static void
test_file_notifier_crawling_recursive (TestCommonContext *fixture,
				       gconstpointer      data)
{
	FilesystemOperation expected_results[] = {
		{ OPERATION_CREATE, "recursive", NULL },
		{ OPERATION_CREATE, "recursive/folder", NULL },
		{ OPERATION_CREATE, "recursive/folder/aaa", NULL },
		{ OPERATION_CREATE, "recursive/bbb", NULL },
	};

	CREATE_FOLDER (fixture, "recursive/folder");
	CREATE_UPDATE_FILE (fixture, "recursive/folder/aaa");
	CREATE_UPDATE_FILE (fixture, "recursive/bbb");

	test_common_context_index_dir (fixture, "recursive",
	                               TRACKER_DIRECTORY_FLAG_RECURSE);

	tracker_file_notifier_start (fixture->notifier);

	test_common_context_expect_results (fixture, expected_results,
					    G_N_ELEMENTS (expected_results),
					    2, TRUE);

	tracker_file_notifier_stop (fixture->notifier);
}

static void
test_file_notifier_crawling_non_recursive_within_recursive (TestCommonContext *fixture,
							    gconstpointer      data)
{
	FilesystemOperation expected_results[] = {
		{ OPERATION_CREATE, "recursive", NULL },
		{ OPERATION_CREATE, "recursive/folder", NULL },
		{ OPERATION_CREATE, "recursive/folder/aaa", NULL },
		{ OPERATION_CREATE, "recursive/bbb", NULL },
		{ OPERATION_CREATE, "recursive/folder/non-recursive", NULL },
		{ OPERATION_CREATE, "recursive/folder/non-recursive/ccc", NULL },
		{ OPERATION_CREATE, "recursive/folder/non-recursive/folder", NULL },
	};

	CREATE_FOLDER (fixture, "recursive/folder");
	CREATE_UPDATE_FILE (fixture, "recursive/folder/aaa");
	CREATE_UPDATE_FILE (fixture, "recursive/bbb");
	CREATE_FOLDER (fixture, "recursive/folder/non-recursive");
	CREATE_UPDATE_FILE (fixture, "recursive/folder/non-recursive/ccc");
	CREATE_FOLDER (fixture, "recursive/folder/non-recursive/folder");
	CREATE_UPDATE_FILE (fixture, "recursive/folder/non-recursive/folder/ddd");

	test_common_context_index_dir (fixture, "recursive",
	                               TRACKER_DIRECTORY_FLAG_RECURSE);
	test_common_context_index_dir (fixture, "recursive/folder/non-recursive",
	                               TRACKER_DIRECTORY_FLAG_NONE);

	tracker_file_notifier_start (fixture->notifier);

	test_common_context_expect_results (fixture, expected_results,
					    G_N_ELEMENTS (expected_results),
					    2, TRUE);

	tracker_file_notifier_stop (fixture->notifier);
}

static void
test_file_notifier_crawling_recursive_within_non_recursive (TestCommonContext *fixture,
							    gconstpointer      data)
{
	FilesystemOperation expected_results[] = {
		{ OPERATION_CREATE, "non-recursive", NULL },
		{ OPERATION_CREATE, "non-recursive/folder", NULL },
		{ OPERATION_CREATE, "non-recursive/bbb", NULL },
		{ OPERATION_CREATE, "non-recursive/folder/recursive", NULL },
		{ OPERATION_CREATE, "non-recursive/folder/recursive/ccc", NULL },
		{ OPERATION_CREATE, "non-recursive/folder/recursive/folder", NULL },
		{ OPERATION_CREATE, "non-recursive/folder/recursive/folder/ddd", NULL },
	};

	CREATE_FOLDER (fixture, "non-recursive/folder");
	CREATE_UPDATE_FILE (fixture, "non-recursive/folder/aaa");
	CREATE_UPDATE_FILE (fixture, "non-recursive/bbb");
	CREATE_FOLDER (fixture, "non-recursive/folder/recursive");
	CREATE_UPDATE_FILE (fixture, "non-recursive/folder/recursive/ccc");
	CREATE_FOLDER (fixture, "non-recursive/folder/recursive/folder");
	CREATE_UPDATE_FILE (fixture, "non-recursive/folder/recursive/folder/ddd");

	test_common_context_index_dir (fixture, "non-recursive/folder/recursive",
	                               TRACKER_DIRECTORY_FLAG_RECURSE);
	test_common_context_index_dir (fixture, "non-recursive",
	                               TRACKER_DIRECTORY_FLAG_NONE);

	tracker_file_notifier_start (fixture->notifier);

	test_common_context_expect_results (fixture, expected_results,
					    G_N_ELEMENTS (expected_results),
					    2, TRUE);

	tracker_file_notifier_stop (fixture->notifier);
}

gint
main (gint    argc,
      gchar **argv)
{
	g_type_init ();
	g_test_init (&argc, &argv, NULL);

	g_test_message ("Testing file notifier");

	test_add ("/libtracker-miner/file-notifier/crawling-non-recursive",
	          test_file_notifier_crawling_non_recursive);
	test_add ("/libtracker-miner/file-notifier/crawling-recursive",
	          test_file_notifier_crawling_recursive);
	test_add ("/libtracker-miner/file-notifier/crawling-non-recursive-within-recursive",
	          test_file_notifier_crawling_non_recursive_within_recursive);
	test_add ("/libtracker-miner/file-notifier/crawling-non-recursive-within-recursive",
	          test_file_notifier_crawling_recursive_within_non_recursive);

	return g_test_run ();
}
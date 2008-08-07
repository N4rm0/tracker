#include <glib.h>
#include <glib/gtestutils.h>
#include <tracker-test-helpers.h>
#include <gio/gio.h>

#include "tracker-db-index.h"

/* From libtracker-common/tracker-config.c */
#define DEFAULT_MAX_BUCKET_COUNT		 524288
#define DEFAULT_MIN_BUCKET_COUNT		 65536

static void
test_get_suggestion ()
{
        TrackerDBIndex *index;
        gchar          *suggestion;

        index = tracker_db_index_new ("./example.index", 
                                      DEFAULT_MIN_BUCKET_COUNT,
                                      DEFAULT_MAX_BUCKET_COUNT,
                                      TRUE);
        
        g_assert (tracker_db_index_get_reload (index));
        
        suggestion = tracker_db_index_get_suggestion (index, "Thiz", 9);
        
        g_assert (tracker_test_helpers_cmpstr_equal (suggestion, "this"));
        
        g_free (suggestion);
        
        g_object_unref (index);
}

static void
test_reloading ()
{
        TrackerDBIndex   *index;
        TrackerIndexItem *hits;
        guint             count;

        index = tracker_db_index_new ("./example.index", 
                                      DEFAULT_MIN_BUCKET_COUNT,
                                      DEFAULT_MAX_BUCKET_COUNT,
                                      TRUE);
        
        tracker_db_index_set_reload (index, TRUE);
        g_assert (tracker_db_index_get_reload (index)); /* Trivial check of get/set */

        if (g_test_trap_fork (0, G_TEST_TRAP_SILENCE_STDERR)) {
                hits = tracker_db_index_get_word_hits (index, "this", &count);
                g_free (hits);
        }

        g_test_trap_assert_stderr ("*Opening index:'./example.index'*");
}

static void
test_bad_index ()
{
        TrackerDBIndex *index;
        guint           count;

        index = tracker_db_index_new ("unknown-index",
                                      DEFAULT_MIN_BUCKET_COUNT,
                                      DEFAULT_MAX_BUCKET_COUNT,
                                      TRUE);

        /* Reload true: lazy opening */
        g_assert (tracker_db_index_get_reload (index));

        /* Return NULL, the index cannot reload the file */
        g_assert (!tracker_db_index_get_word_hits (index, "this", &count));

        /* Return NULL, the index cannot reload the file */
        g_assert (!tracker_db_index_get_suggestion (index, "Thiz", 9));

}

static void
test_created_file_in_the_mean_time ()
{
        TrackerDBIndex *index;
        GFile          *good, *bad;
        guint           count;

        index = tracker_db_index_new ("./unknown-index",
                                      DEFAULT_MIN_BUCKET_COUNT,
                                      DEFAULT_MAX_BUCKET_COUNT,
                                      TRUE);

        /* Reload true: Lazy opening */
        g_assert (tracker_db_index_get_reload (index));

        good = g_file_new_for_path ("./example.index");
        bad = g_file_new_for_path ("./unknown-index");

        g_file_copy (good, bad, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);

        /* Now the first operation reload the index */
        g_assert (tracker_db_index_get_word_hits (index, "this", &count));
        
        /* Reload false: It is already reloaded */
        g_assert (!tracker_db_index_get_reload (index));

        g_file_delete (bad, NULL, NULL);
}


int
main (int argc, char **argv) {

        int result;

	g_type_init ();
        g_thread_init (NULL);
	g_test_init (&argc, &argv, NULL);

        /* Init */

        g_test_add_func ("/trackerd/tracker-indexer/get_suggestion",
                         test_get_suggestion );
        g_test_add_func ("/trackerd/tracker-indexer/reloading",
                         test_reloading );
        g_test_add_func ("/trackerd/tracker-indexer/bad_index",
                         test_bad_index );

        result = g_test_run ();
        
        /* End */

        return result;
}

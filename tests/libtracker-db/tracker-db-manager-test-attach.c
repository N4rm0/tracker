/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* 
 * Copyright (C) 2008, Nokia (urho.konttori@nokia.com)
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
#include <glib.h>
#include <glib/gtestutils.h>

#include <libtracker-db/tracker-db-manager.h>
#include "tracker-db-manager-common.h"


typedef enum {
        NO_INIT,
        INIT_NO_REINDEX,
        INIT_REINDEX
} Status;

static gboolean db_manager_status = NO_INIT;

void 
ensure_db_manager_is_reindex (gboolean must_reindex)
{
        gint first;

        if (db_manager_status == NO_INIT) {
                if (must_reindex) {
                        tracker_db_manager_init (TRACKER_DB_MANAGER_ATTACH_ALL |TRACKER_DB_MANAGER_FORCE_REINDEX,
                                                 &first);
                        db_manager_status = INIT_REINDEX;
                } else {
                        tracker_db_manager_init (TRACKER_DB_MANAGER_ATTACH_ALL,
                                                 &first);
                        db_manager_status = INIT_NO_REINDEX;
                }
                return;
        }

        if (db_manager_status == INIT_NO_REINDEX && !must_reindex) {
                // tracker_db_manager is already correctly initialised
                return;
        }

        if (db_manager_status == INIT_REINDEX && must_reindex) {
                // tracker_db_manager is already correctly initialised
                return ;
        }

        tracker_db_manager_shutdown (must_reindex);
        if (must_reindex) {
                tracker_db_manager_init (TRACKER_DB_MANAGER_ATTACH_ALL |TRACKER_DB_MANAGER_FORCE_REINDEX,
                                         &first);
                db_manager_status = INIT_REINDEX;
        } else {
                tracker_db_manager_init (TRACKER_DB_MANAGER_ATTACH_ALL,
                                         &first);
                db_manager_status = INIT_NO_REINDEX;
        }
}





void
test_assert_tables_in_db (TrackerDB db, gchar *query) 
{
        g_assert (test_assert_query_run (db, query));
}

static void
test_always_same_iface_no_reindex () 
{
        TrackerDBInterface *common, *xesam;
        
        ensure_db_manager_is_reindex (FALSE);
 
        common = tracker_db_manager_get_db_interface (TRACKER_DB_COMMON);
        xesam = tracker_db_manager_get_db_interface (TRACKER_DB_XESAM);

        /* The pointer must be the same */
        g_assert (common == xesam);
        

}

static void
test_creation_common_db_no_reindex () 
{
        ensure_db_manager_is_reindex (FALSE);
        test_assert_tables_in_db (TRACKER_DB_COMMON, "SELECT * FROM MetaDataTypes");
}


static void
test_creation_xesam_db_no_reindex_multiple_interfaces () 
{
        TrackerDBInterface *iface;

        ensure_db_manager_is_reindex (FALSE);

        iface = tracker_db_manager_get_db_interfaces (2, 
        											  TRACKER_DB_XESAM, 
        											  TRACKER_DB_COMMON);

        test_assert_query_run_on_iface (iface, "SELECT * FROM XesamServiceTypes");
}


static void
test_creation_xesam_db_no_reindex () 
{
        ensure_db_manager_is_reindex (FALSE);
        test_assert_tables_in_db (TRACKER_DB_XESAM, "SELECT * FROM XesamServiceTypes");
}

static void
test_creation_file_meta_db_no_reindex ()
{
        ensure_db_manager_is_reindex (FALSE);
        test_assert_tables_in_db (TRACKER_DB_COMMON, "SELECT * FROM 'file-meta'.ServiceMetaData");
}

static void
test_creation_file_contents_db_no_reindex ()
{
        ensure_db_manager_is_reindex (FALSE);
        test_assert_tables_in_db (TRACKER_DB_FILE_CONTENTS, "SELECT * FROM 'file-contents'.ServiceContents");
}


int
main (int argc, char **argv) {

        int result;

	g_type_init ();
        g_thread_init (NULL);
	g_test_init (&argc, &argv, NULL);


        // Tests with attach and no-reindex
        g_test_add_func ("/libtrakcer-db/tracker-db-manager/attach/no-reindex/equal_iface",
                         test_always_same_iface_no_reindex);

        g_test_add_func ("/libtracker-db/tracker-db-manager/attach/no-reindex/common_db_tables",
                        test_creation_common_db_no_reindex);

        g_test_add_func ("/libtracker-db/tracker-db-manager/attach/no-reindex/xesam_db_tables",
                         test_creation_xesam_db_no_reindex);

        g_test_add_func ("/libtracker-db/tracker-db-manager/attach/no-reindex/xesam_db_tables/multiple_interfaces",
                         test_creation_xesam_db_no_reindex_multiple_interfaces);

        g_test_add_func ("/libtracker-db/tracker-db-manager/attach/no-reindex/file_meta_db_tables",
                         test_creation_file_meta_db_no_reindex);

        g_test_add_func ("/libtracker-db/tracker-db-manager/attach/no-reindex/file_contents_db_tables",
                         test_creation_file_contents_db_no_reindex);


        result = g_test_run ();
        
        /* End */

        return result;
}

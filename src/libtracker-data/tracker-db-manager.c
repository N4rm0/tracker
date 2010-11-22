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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include "config.h"

#include <sys/types.h>
#include <pwd.h>

#include <string.h>
#include <stdlib.h>
#include <regex.h>
#include <zlib.h>
#include <locale.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#include <glib/gstdio.h>

#include <libtracker-common/tracker-date-time.h>
#include <libtracker-common/tracker-file-utils.h>
#include <libtracker-common/tracker-utils.h>

#if HAVE_TRACKER_FTS
#include <libtracker-fts/tracker-fts.h>
#endif

#include "tracker-db-journal.h"
#include "tracker-db-manager.h"
#include "tracker-db-interface-sqlite.h"
#include "tracker-db-interface.h"

/* ZLib buffer settings */
#define ZLIB_BUF_SIZE                 8192

/* Required minimum space needed to create databases (5Mb) */
#define TRACKER_DB_MIN_REQUIRED_SPACE 5242880

/* Default memory settings for databases */
#define TRACKER_DB_PAGE_SIZE_DONT_SET -1

/* Set current database version we are working with */
#define TRACKER_DB_VERSION_NOW        TRACKER_DB_VERSION_0_9_24
#define TRACKER_DB_VERSION_FILE       "db-version.txt"
#define TRACKER_DB_LOCALE_FILE        "db-locale.txt"

#define IN_USE_FILENAME               ".meta.isrunning"

/* Stamp files to know crawling/indexing state */
#define FIRST_INDEX_FILENAME          "first-index.txt"
#define LAST_CRAWL_FILENAME           "last-crawl.txt"

typedef enum {
	TRACKER_DB_LOCATION_DATA_DIR,
	TRACKER_DB_LOCATION_USER_DATA_DIR,
	TRACKER_DB_LOCATION_SYS_TMP_DIR,
} TrackerDBLocation;

typedef enum {
	TRACKER_DB_VERSION_UNKNOWN, /* Unknown */
	TRACKER_DB_VERSION_0_6_6,   /* before indexer-split */
	TRACKER_DB_VERSION_0_6_90,  /* after  indexer-split */
	TRACKER_DB_VERSION_0_6_91,  /* stable release */
	TRACKER_DB_VERSION_0_6_92,  /* current TRUNK */
	TRACKER_DB_VERSION_0_7_0,   /* vstore branch */
	TRACKER_DB_VERSION_0_7_4,   /* nothing special */
	TRACKER_DB_VERSION_0_7_12,  /* nmo ontology */
	TRACKER_DB_VERSION_0_7_13,  /* coalesce & writeback */
	TRACKER_DB_VERSION_0_7_17,  /* mlo ontology */
	TRACKER_DB_VERSION_0_7_20,  /* nco im ontology */
	TRACKER_DB_VERSION_0_7_21,  /* named graphs/localtime */
	TRACKER_DB_VERSION_0_7_22,  /* fts-limits branch */
	TRACKER_DB_VERSION_0_7_28,  /* RC1 + mto + nco:url */
	TRACKER_DB_VERSION_0_8_0,   /* stable release */
	TRACKER_DB_VERSION_0_9_0,   /* unstable release */
	TRACKER_DB_VERSION_0_9_8,   /* affiliation cardinality + volumes */
	TRACKER_DB_VERSION_0_9_15,  /* mtp:hidden */
	TRACKER_DB_VERSION_0_9_16,  /* Fix for NB#184823 */
	TRACKER_DB_VERSION_0_9_19,  /* collation */
	TRACKER_DB_VERSION_0_9_21,  /* Fix for NB#186055 */
	TRACKER_DB_VERSION_0_9_24   /* nmo:PhoneMessage class */
} TrackerDBVersion;

typedef struct {
	TrackerDB           db;
	TrackerDBLocation   location;
	TrackerDBInterface *iface;
	const gchar        *file;
	const gchar        *name;
	gchar              *abs_filename;
	gint                cache_size;
	gint                page_size;
	gboolean            attached;
	gboolean            is_index;
	guint64             mtime;
} TrackerDBDefinition;

static TrackerDBDefinition dbs[] = {
	{ TRACKER_DB_UNKNOWN,
	  TRACKER_DB_LOCATION_USER_DATA_DIR,
	  NULL,
	  NULL,
	  NULL,
	  NULL,
	  32,
	  TRACKER_DB_PAGE_SIZE_DONT_SET,
	  FALSE,
	  FALSE,
	  0 },
	{ TRACKER_DB_METADATA,
	  TRACKER_DB_LOCATION_DATA_DIR,
	  NULL,
	  "meta.db",
	  "meta",
	  NULL,
	  TRACKER_DB_CACHE_SIZE_DEFAULT,
	  8192,
	  FALSE,
	  FALSE,
	  0 },
};

static gboolean            db_exec_no_reply    (TrackerDBInterface *iface,
                                                const gchar        *query,
                                                ...);
static TrackerDBInterface *db_interface_create (TrackerDB           db);
static TrackerDBInterface *tracker_db_manager_get_db_interfaces     (gint num, ...);
static TrackerDBInterface *tracker_db_manager_get_db_interfaces_ro  (gint num, ...);
static void                db_remove_locale_file  (void);

static gboolean              initialized;
static gboolean              locations_initialized;
static gchar                *sql_dir;
static gchar                *data_dir = NULL;
static gchar                *user_data_dir = NULL;
static gchar                *sys_tmp_dir = NULL;
static gchar                *in_use_filename = NULL;
static gpointer              db_type_enum_class_pointer;
static TrackerDBManagerFlags old_flags = 0;
static guint                 s_cache_size;
static guint                 u_cache_size;
static gchar                *transient_filename = NULL;

static GStaticPrivate        interface_data_key = G_STATIC_PRIVATE_INIT;

static const gchar *
location_to_directory (TrackerDBLocation location)
{
	switch (location) {
	case TRACKER_DB_LOCATION_DATA_DIR:
		return data_dir;
	case TRACKER_DB_LOCATION_USER_DATA_DIR:
		return user_data_dir;
	case TRACKER_DB_LOCATION_SYS_TMP_DIR:
		return sys_tmp_dir;
	default:
		return NULL;
	};
}

static gboolean
db_exec_no_reply (TrackerDBInterface *iface,
                  const gchar        *query,
                  ...)
{
	TrackerDBResultSet *result_set;
	va_list                     args;

	va_start (args, query);
	result_set = tracker_db_interface_execute_vquery (iface, NULL, query, args);
	va_end (args);

	if (result_set) {
		g_object_unref (result_set);
	}

	return TRUE;
}

TrackerDBManagerFlags
tracker_db_manager_get_flags (guint *select_cache_size, guint *update_cache_size)
{
	if (select_cache_size)
		*select_cache_size = s_cache_size;

	if (update_cache_size)
		*update_cache_size = u_cache_size;

	return old_flags;
}

static void
db_set_params (TrackerDBInterface *iface,
               gint                cache_size,
               gint                page_size)
{
	gchar *queries = NULL;
	const gchar *pragmas_file;

	pragmas_file = g_getenv ("TRACKER_PRAGMAS_FILE");

	if (pragmas_file && g_file_get_contents (pragmas_file, &queries, NULL, NULL)) {
		gchar *query;
		g_debug ("PRAGMA's from file: %s", pragmas_file);
		query = strtok (queries, "\n");
		while (query) {
			g_debug ("  INIT query: %s", query);
			tracker_db_interface_execute_query (iface, NULL, "%s", query);
			query = strtok (NULL, "\n");
		}
		g_free (queries);
	} else {
		TrackerDBResultSet *result_set;

		tracker_db_interface_execute_query (iface, NULL, "PRAGMA synchronous = OFF;");
		tracker_db_interface_execute_query (iface, NULL, "PRAGMA count_changes = 0;");
		tracker_db_interface_execute_query (iface, NULL, "PRAGMA temp_store = FILE;");
		tracker_db_interface_execute_query (iface, NULL, "PRAGMA encoding = \"UTF-8\"");
		tracker_db_interface_execute_query (iface, NULL, "PRAGMA auto_vacuum = 0;");

		result_set = tracker_db_interface_execute_query (iface, NULL, "PRAGMA journal_mode = WAL;");
		if (result_set == NULL) {
			/* Don't just silence the problem. This pragma must return 'WAL' */
			g_message ("Can't set journal mode to WAL");
		} else {
			g_object_unref (result_set);
		}

		if (page_size != TRACKER_DB_PAGE_SIZE_DONT_SET) {
			g_message ("  Setting page size to %d", page_size);
			tracker_db_interface_execute_query (iface, NULL, "PRAGMA page_size = %d", page_size);
		}

		tracker_db_interface_execute_query (iface, NULL, "PRAGMA cache_size = %d", cache_size);
		g_message ("  Setting cache size to %d", cache_size);
	}
}


static const gchar *
db_type_to_string (TrackerDB db)
{
	GType       type;
	GEnumClass *enum_class;
	GEnumValue *enum_value;

	type = tracker_db_get_type ();
	enum_class = G_ENUM_CLASS (g_type_class_peek (type));
	enum_value = g_enum_get_value (enum_class, db);

	if (!enum_value) {
		return "unknown";
	}

	return enum_value->value_nick;
}

static TrackerDBInterface *
db_interface_get (TrackerDB  type,
                  gboolean  *create)
{
	TrackerDBInterface *iface;
	const gchar        *path;

	path = dbs[type].abs_filename;

	if (!g_file_test (path, G_FILE_TEST_EXISTS)) {
		*create = TRUE;
	} else {
		*create = FALSE;
	}

	g_message ("%s database... '%s' (%s)",
	           *create ? "Creating" : "Loading",
	           path,
	           db_type_to_string (type));

	iface = tracker_db_interface_sqlite_new (path, transient_filename);

	db_set_params (iface,
	               dbs[type].cache_size,
	               dbs[type].page_size);

	db_exec_no_reply (iface,
	                  "ATTACH '%s' as 'transient'",
	                  transient_filename);

	return iface;
}

static TrackerDBInterface *
db_interface_get_metadata (void)
{
	TrackerDBInterface *iface;
	gboolean            create;

	iface = db_interface_get (TRACKER_DB_METADATA, &create);

	return iface;
}

static TrackerDBInterface *
db_interface_create (TrackerDB db)
{
	switch (db) {
	case TRACKER_DB_UNKNOWN:
		return NULL;

	case TRACKER_DB_METADATA:
		return db_interface_get_metadata ();

	default:
		g_critical ("This TrackerDB type:%d->'%s' has no interface set up yet!!",
		            db,
		            db_type_to_string (db));
		return NULL;
	}
}

static void
db_manager_remove_journal (void)
{
	gchar *path;
	gchar *directory, *rotate_to = NULL;
	gsize chunk_size;
	gboolean do_rotate = FALSE;
	const gchar *dirs[3] = { NULL, NULL, NULL };
	guint i;

	/* We duplicate the path here because later we shutdown the
	 * journal which frees this data. We want to survive that.
	 */
	path = g_strdup (tracker_db_journal_get_filename ());
	if (!path) {
		return;
	}

	g_message ("  Removing journal:'%s'", path);

	directory = g_path_get_dirname (path);

	tracker_db_journal_get_rotating (&do_rotate, &chunk_size, &rotate_to);
	tracker_db_journal_shutdown ();

	dirs[0] = directory;
	dirs[1] = do_rotate ? rotate_to : NULL;

	for (i = 0; dirs[i] != NULL; i++) {
		GDir *journal_dir;
		const gchar *f;

		journal_dir = g_dir_open (dirs[i], 0, NULL);
		if (!journal_dir) {
			continue;
		}

		/* Remove rotated chunks */
		while ((f = g_dir_read_name (journal_dir)) != NULL) {
			gchar *fullpath;

			if (!g_str_has_prefix (f, TRACKER_DB_JOURNAL_FILENAME ".")) {
				continue;
			}

			fullpath = g_build_filename (dirs[i], f, NULL);
			if (g_unlink (fullpath) == -1) {
				g_message ("%s", g_strerror (errno));
			}
			g_free (fullpath);
		}

		g_dir_close (journal_dir);
	}

	g_free (rotate_to);
	g_free (directory);

	/* Remove active journal */
	if (g_unlink (path) == -1) {
		g_message ("%s", g_strerror (errno));
	}
	g_free (path);
}

static void
db_manager_remove_all (gboolean rm_journal)
{
	guint i;

	g_message ("Removing all database/storage files");

	/* Remove stamp files */
	tracker_db_manager_set_first_index_done (FALSE);
	tracker_db_manager_set_last_crawl_done (FALSE);

	/* NOTE: We don't have to be initialized for this so we
	 * calculate the absolute directories here.
	 */
	for (i = 1; i < G_N_ELEMENTS (dbs); i++) {
		gchar *filename;

		g_message ("  Removing database:'%s'", dbs[i].abs_filename);
		g_unlink (dbs[i].abs_filename);

		/* also delete shm and wal helper files */
		filename = g_strdup_printf ("%s-shm", dbs[i].abs_filename);
		g_unlink (filename);
		g_free (filename);

		filename = g_strdup_printf ("%s-wal", dbs[i].abs_filename);
		g_unlink (filename);
		g_free (filename);
	}

	if (rm_journal) {


		db_manager_remove_journal ();

		/* If also the journal is gone, we can also remove db-version.txt, it
		 * would have no more relevance whatsoever. */
		tracker_db_manager_remove_version_file ();
	}

	/* Remove locale file also */
	db_remove_locale_file ();
}

void
tracker_db_manager_remove_version_file (void)
{
	gchar *filename;

	filename = g_build_filename (data_dir, TRACKER_DB_VERSION_FILE, NULL);
	g_message ("  Removing db-version file:'%s'", filename);
	g_unlink (filename);
	g_free (filename);
}

static TrackerDBVersion
db_get_version (void)
{
	TrackerDBVersion  version;
	gchar            *filename;

	filename = g_build_filename (data_dir, TRACKER_DB_VERSION_FILE, NULL);

	if (G_LIKELY (g_file_test (filename, G_FILE_TEST_EXISTS))) {
		gchar *contents;

		/* Check version is correct */
		if (G_LIKELY (g_file_get_contents (filename, &contents, NULL, NULL))) {
			if (contents && strlen (contents) <= 2) {
				version = atoi (contents);
			} else {
				g_message ("  Version file content size is either 0 or bigger than expected");

				version = TRACKER_DB_VERSION_UNKNOWN;
			}

			g_free (contents);
		} else {
			g_message ("  Could not get content of file '%s'", filename);

			version = TRACKER_DB_VERSION_UNKNOWN;
		}
	} else {
		g_message ("  Could not find database version file:'%s'", filename);
		g_message ("  Current databases are either old or no databases are set up yet");

		version = TRACKER_DB_VERSION_UNKNOWN;
	}

	g_free (filename);

	return version;
}

static void
db_set_version (void)
{
	GError *error = NULL;
	gchar  *filename;
	gchar  *str;

	filename = g_build_filename (data_dir, TRACKER_DB_VERSION_FILE, NULL);
	g_message ("  Creating version file '%s'", filename);

	str = g_strdup_printf ("%d", TRACKER_DB_VERSION_NOW);

	if (!g_file_set_contents (filename, str, -1, &error)) {
		g_message ("  Could not set file contents, %s",
		           error ? error->message : "no error given");
		g_clear_error (&error);
	}

	g_free (str);
	g_free (filename);
}

static void
db_remove_locale_file (void)
{
	gchar *filename;

	filename = g_build_filename (data_dir, TRACKER_DB_LOCALE_FILE, NULL);
	g_message ("  Removing db-locale file:'%s'", filename);
	g_unlink (filename);
	g_free (filename);
}

static gchar *
db_get_locale (void)
{
	gchar *locale = NULL;
	gchar *filename;

	filename = g_build_filename (data_dir, TRACKER_DB_LOCALE_FILE, NULL);

	if (G_LIKELY (g_file_test (filename, G_FILE_TEST_EXISTS))) {
		gchar *contents;

		/* Check locale is correct */
		if (G_LIKELY (g_file_get_contents (filename, &contents, NULL, NULL))) {
			if (contents && strlen (contents) == 0) {
				g_critical ("  Empty locale file found at '%s'", filename);
				g_free (contents);
			} else {
				/* Re-use contents */
				locale = contents;
			}
		} else {
			g_critical ("  Could not get content of file '%s'", filename);
		}
	} else {
		g_critical ("  Could not find database locale file:'%s'", filename);
	}

	g_free (filename);

	return locale;
}

static void
db_set_locale (const gchar *locale)
{
	GError *error = NULL;
	gchar  *filename;
	gchar  *str;

	filename = g_build_filename (data_dir, TRACKER_DB_LOCALE_FILE, NULL);
	g_message ("  Creating locale file '%s'", filename);

	str = g_strdup_printf ("%s", locale ? locale : "");

	if (!g_file_set_contents (filename, str, -1, &error)) {
		g_message ("  Could not set file contents, %s",
		           error ? error->message : "no error given");
		g_clear_error (&error);
	}

	g_free (str);
	g_free (filename);
}

gboolean
tracker_db_manager_locale_changed (void)
{
	gchar *db_locale;
	gchar *current_locale;
	gboolean changed;

	/* Get current collation locale */
	current_locale = setlocale (LC_COLLATE, NULL);

	/* Get db locale */
	db_locale = db_get_locale ();

	/* If they are different, recreate indexes. Note that having
	 * both to NULL is actually valid, they would default to
	 * the unicode collation without locale-specific stuff. */
	if (g_strcmp0 (db_locale, current_locale) != 0) {
		g_message ("Locale change detected from '%s' to '%s'...",
		           db_locale, current_locale);
		changed = TRUE;
	} else {
		g_message ("Current and DB locales match: '%s'", db_locale);
		changed = FALSE;
	}

	g_free (db_locale);
	return changed;
}

void
tracker_db_manager_set_current_locale (void)
{
	const gchar *current_locale;

	/* Get current collation locale */
	current_locale = setlocale (LC_COLLATE, NULL);

	g_message ("Changing db locale to: '%s'", current_locale);

	db_set_locale (current_locale);
}

static void
db_manager_analyze (TrackerDB           db,
                    TrackerDBInterface *iface)
{
	guint64             current_mtime;

	current_mtime = tracker_file_get_mtime (dbs[db].abs_filename);

	if (current_mtime > dbs[db].mtime) {
		g_message ("  Analyzing DB:'%s'", dbs[db].name);
		db_exec_no_reply (iface, "ANALYZE %s.Services", dbs[db].name);

		/* Remember current mtime for future */
		dbs[db].mtime = current_mtime;
	} else {
		g_message ("  Not updating DB:'%s', no changes since last optimize", dbs[db].name);
	}
}

GType
tracker_db_get_type (void)
{
	static GType etype = 0;

	if (etype == 0) {
		static const GEnumValue values[] = {
			{ TRACKER_DB_METADATA,
			  "TRACKER_DB_METADATA",
			  "metadata" },
		};

		etype = g_enum_register_static ("TrackerDB", values);
	}

	return etype;
}

static void
db_recreate_all (void)
{
	guint i;

	/* We call an internal version of this function here
	 * because at the time 'initialized' = FALSE and that
	 * will cause errors and do nothing.
	 */
	g_message ("Cleaning up database files for reindex");

	db_manager_remove_all (FALSE);

	/* Now create the databases and close them */
	g_message ("Creating database files, this may take a few moments...");

	for (i = 1; i < G_N_ELEMENTS (dbs); i++) {
		dbs[i].iface = db_interface_create (i);
	}

	/* We don't close the dbs in the same loop as before
	 * becase some databases need other databases
	 * attached to be created correctly.
	 */
	for (i = 1; i < G_N_ELEMENTS (dbs); i++) {
		g_object_unref (dbs[i].iface);
		dbs[i].iface = NULL;
	}

	/* Initialize locale file */
	db_set_locale (setlocale (LC_COLLATE, NULL));
}

static void
tracker_db_manager_prepare_transient (gboolean readonly)
{
	gchar *shm_path;
	struct passwd *pwd;

	shm_path = g_build_filename (G_DIR_SEPARATOR_S "dev", "shm", NULL);

	g_free (transient_filename);

	if (g_file_test (shm_path, G_FILE_TEST_IS_DIR)) {
		g_free (shm_path);
		shm_path = g_build_filename (G_DIR_SEPARATOR_S "dev", "shm", g_get_user_name (), NULL);
		transient_filename = g_build_filename (shm_path,
		                                       "tracker-transient.db",
		                                       NULL);
	} else {
		g_free (shm_path);
		shm_path = g_build_path (g_get_tmp_dir (), g_get_user_name (), NULL);
		transient_filename = g_build_filename (g_get_tmp_dir (),
		                                       g_get_user_name (),
		                                       "tracker-transient.db",
		                                       NULL);
	}

	if (!readonly) {
		if (!g_file_test (shm_path, G_FILE_TEST_IS_DIR)) {
			g_mkdir_with_parents (shm_path, S_IRUSR | S_IWUSR | S_IXUSR |
			                                S_IRGRP | S_IWGRP | S_IXGRP);
		} else {
			g_chmod (shm_path, S_IRUSR | S_IWUSR | S_IXUSR |
			                   S_IRGRP | S_IWGRP | S_IXGRP);
		}

		pwd = getpwnam (g_get_user_name ());

		if (pwd != NULL) {
			chown (shm_path, pwd->pw_uid, -1);
		}
	}

	g_free (shm_path);
}

void
tracker_db_manager_init_locations (void)
{
	const gchar *dir;
	guint i;
	gchar *filename;

	filename = g_strdup_printf ("tracker-%s", g_get_user_name ());
	sys_tmp_dir = g_build_filename (g_get_tmp_dir (), filename, NULL);
	g_free (filename);

	user_data_dir = g_build_filename (g_get_user_data_dir (),
	                                  "tracker",
	                                  "data",
	                                  NULL);

	data_dir = g_build_filename (g_get_user_cache_dir (),
	                             "tracker",
	                             NULL);

	for (i = 1; i < G_N_ELEMENTS (dbs); i++) {
		dir = location_to_directory (dbs[i].location);
		dbs[i].abs_filename = g_build_filename (dir, dbs[i].file, NULL);
	}

	tracker_db_manager_prepare_transient (TRUE);

	locations_initialized = TRUE;
}

gboolean
tracker_db_manager_init (TrackerDBManagerFlags  flags,
                         gboolean              *first_time,
                         gboolean               shared_cache,
                         guint                  select_cache_size,
                         guint                  update_cache_size,
                         TrackerBusyCallback    busy_callback,
                         gpointer               busy_user_data)
{
	GType               etype;
	TrackerDBVersion    version;
	gchar              *filename;
	const gchar        *dir;
	const gchar        *env_path;
	gboolean            need_reindex;
	guint               i;
	int                 in_use_file;
	gboolean            loaded = FALSE;
	TrackerDBInterface *resources_iface;

	/* First set defaults for return values */
	if (first_time) {
		*first_time = FALSE;
	}

	if (initialized) {
		return TRUE;
	}

	need_reindex = FALSE;

	/* Since we don't reference this enum anywhere, we do
	 * it here to make sure it exists when we call
	 * g_type_class_peek(). This wouldn't be necessary if
	 * it was a param in a GObject for example.
	 *
	 * This does mean that we are leaking by 1 reference
	 * here and should clean it up, but it doesn't grow so
	 * this is acceptable.
	 */
	etype = tracker_db_get_type ();
	db_type_enum_class_pointer = g_type_class_ref (etype);

	/* Set up locations */
	g_message ("Setting database locations");

	old_flags = flags;

	filename = g_strdup_printf ("tracker-%s", g_get_user_name ());
	g_free (sys_tmp_dir);
	sys_tmp_dir = g_build_filename (g_get_tmp_dir (), filename, NULL);
	g_free (filename);

	env_path = g_getenv ("TRACKER_DB_SQL_DIR");

	if (G_UNLIKELY (!env_path)) {
		sql_dir = g_build_filename (SHAREDIR,
		                            "tracker",
		                            NULL);
	} else {
		sql_dir = g_strdup (env_path);
	}

	g_free (user_data_dir);
	user_data_dir = g_build_filename (g_get_user_data_dir (),
	                                  "tracker",
	                                  "data",
	                                  NULL);

	g_free (data_dir);
	data_dir = g_build_filename (g_get_user_cache_dir (),
	                             "tracker",
	                             NULL);

	in_use_filename = g_build_filename (g_get_user_data_dir (),
	                                    "tracker",
	                                    "data",
	                                    IN_USE_FILENAME,
	                                    NULL);

	/* Make sure the directories exist */
	g_message ("Checking database directories exist");

	g_mkdir_with_parents (data_dir, 00755);
	g_mkdir_with_parents (user_data_dir, 00755);
	g_mkdir_with_parents (sys_tmp_dir, 00755);

	g_message ("Checking database version");

	version = db_get_version ();

	if (version < TRACKER_DB_VERSION_NOW) {
		g_message ("  A reindex will be forced");
		need_reindex = TRUE;
	}

	if (need_reindex) {
		db_set_version ();
	}

	g_message ("Checking database files exist");

	for (i = 1; i < G_N_ELEMENTS (dbs); i++) {
		/* Fill absolute path for the database */
		dir = location_to_directory (dbs[i].location);

		g_free (dbs[i].abs_filename);
		dbs[i].abs_filename = g_build_filename (dir, dbs[i].file, NULL);

		/* Check we have each database in place, if one is
		 * missing, we reindex.
		 */

		/* No need to check for other files not existing (for
		 * reindex) if one is already missing.
		 */
		if (need_reindex) {
			continue;
		}

		if (!g_file_test (dbs[i].abs_filename, G_FILE_TEST_EXISTS)) {
			g_message ("Could not find database file:'%s'", dbs[i].abs_filename);
			g_message ("One or more database files are missing, a reindex will be forced");
			need_reindex = TRUE;
		}
	}

	tracker_db_manager_prepare_transient (flags & TRACKER_DB_MANAGER_READONLY);

	locations_initialized = TRUE;

	/* If we are just initializing to remove the databases,
	 * return here.
	 */
	if ((flags & TRACKER_DB_MANAGER_REMOVE_ALL) != 0) {
		initialized = TRUE;
		return TRUE;
	}

	/* Set general database options */
	if (shared_cache) {
		g_message ("Enabling database shared cache");
		tracker_db_interface_sqlite_enable_shared_cache ();
	}

	/* Should we reindex? If so, just remove all databases files,
	 * NOT the paths, note, that these paths are also used for
	 * other things like the nfs lock file.
	 */
	if (flags & TRACKER_DB_MANAGER_FORCE_REINDEX || need_reindex) {
		if (flags & TRACKER_DB_MANAGER_READONLY) {
			/* no reindexing supported in read-only mode (direct access) */
			return FALSE;
		}

		if (first_time) {
			*first_time = TRUE;
		}

		if (!tracker_file_system_has_enough_space (data_dir, TRACKER_DB_MIN_REQUIRED_SPACE, TRUE)) {
			return FALSE;
		}

		/* Clear the first-index stamp file */
		tracker_db_manager_set_first_index_done (FALSE);

		db_recreate_all ();

		/* Load databases */
		g_message ("Loading databases files...");

	} else if ((flags & TRACKER_DB_MANAGER_READONLY) == 0) {
		/* do not do shutdown check for read-only mode (direct access) */
		gboolean must_recreate;
		gchar *journal_filename;

		/* Load databases */
		g_message ("Loading databases files...");

		journal_filename = g_build_filename (g_get_user_data_dir (),
		                                     "tracker",
		                                     "data",
		                                     TRACKER_DB_JOURNAL_FILENAME,
		                                     NULL);

		must_recreate = !tracker_db_journal_reader_verify_last (journal_filename,
		                                                        NULL);

		g_free (journal_filename);

		if (!must_recreate && g_file_test (in_use_filename, G_FILE_TEST_EXISTS)) {
			gsize size = 0;

			g_message ("Didn't shut down cleanly last time, doing integrity checks");

			for (i = 1; i < G_N_ELEMENTS (dbs) && !must_recreate; i++) {
				struct stat st;
				GError *error = NULL;
				TrackerDBStatement *stmt;

				if (g_stat (dbs[i].abs_filename, &st) == 0) {
					size = st.st_size;
				}

				/* Size is 1 when using echo > file.db, none of our databases
				 * are only one byte in size even initually. */

				if (size <= 1) {
					must_recreate = TRUE;
					continue;
				}

				dbs[i].iface = db_interface_create (i);
				dbs[i].mtime = tracker_file_get_mtime (dbs[i].abs_filename);

				loaded = TRUE;

				tracker_db_interface_set_busy_handler (dbs[i].iface,
				                                       busy_callback,
				                                       "Integrity checking",
				                                       busy_user_data);

				stmt = tracker_db_interface_create_statement (dbs[i].iface, TRACKER_DB_STATEMENT_CACHE_TYPE_NONE, &error,
				                                              "PRAGMA integrity_check(1)");

				if (error != NULL) {
					if (error->domain == TRACKER_DB_INTERFACE_ERROR &&
					    error->code == TRACKER_DB_QUERY_ERROR) {
						must_recreate = TRUE;
					} else {
						g_critical ("%s", error->message);
					}
					g_error_free (error);
				} else {
					TrackerDBCursor *cursor = NULL;

					if (stmt) {
						cursor = tracker_db_statement_start_cursor (stmt, NULL);
						g_object_unref (stmt);
					} else {
						g_critical ("Can't create stmt for integrity_check, no error given");
					}

					if (cursor) {
						if (tracker_db_cursor_iter_next (cursor, NULL, NULL)) {
							if (g_strcmp0 (tracker_db_cursor_get_string (cursor, 0, NULL), "ok") != 0) {
								must_recreate = TRUE;
							}
						}
						g_object_unref (cursor);
					}
				}

				tracker_db_interface_set_busy_handler (dbs[i].iface, NULL, NULL, NULL);
			}
		}

		if (must_recreate) {

			g_message ("Database severely damaged. We will recreate it and replay the journal if available.");

			if (first_time) {
				*first_time = TRUE;
			}

			for (i = 1; i < G_N_ELEMENTS (dbs); i++) {
				if (dbs[i].iface) {
					g_object_unref (dbs[i].iface);
					dbs[i].iface = NULL;
				}
			}

			if (!tracker_file_system_has_enough_space (data_dir, TRACKER_DB_MIN_REQUIRED_SPACE, TRUE)) {
				return FALSE;
			}

			db_recreate_all ();
			loaded = FALSE;
		}

	}

	if (!loaded) {
		for (i = 1; i < G_N_ELEMENTS (dbs); i++) {
			dbs[i].mtime = tracker_file_get_mtime (dbs[i].abs_filename);
		}
	}

	if ((flags & TRACKER_DB_MANAGER_READONLY) == 0) {
		/* do not create in-use file for read-only mode (direct access) */
		in_use_file = g_open (in_use_filename,
			              O_WRONLY | O_APPEND | O_CREAT | O_SYNC,
			              S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);

		if (in_use_file >= 0) {
		        fsync (in_use_file);
		        close (in_use_file);
		}
	}

	initialized = TRUE;

	if (flags & TRACKER_DB_MANAGER_READONLY) {
		resources_iface = tracker_db_manager_get_db_interfaces_ro (1,
		                                                           TRACKER_DB_METADATA);
	} else {
		resources_iface = tracker_db_manager_get_db_interfaces (1,
		                                                        TRACKER_DB_METADATA);
	}

	tracker_db_interface_set_max_stmt_cache_size (resources_iface,
	                                              TRACKER_DB_STATEMENT_CACHE_TYPE_SELECT,
	                                              select_cache_size);

	tracker_db_interface_set_max_stmt_cache_size (resources_iface,
	                                              TRACKER_DB_STATEMENT_CACHE_TYPE_UPDATE,
	                                              update_cache_size);

	s_cache_size = select_cache_size;
	u_cache_size = update_cache_size;

	g_static_private_set (&interface_data_key, resources_iface, (GDestroyNotify) g_object_unref);

	return TRUE;
}

void
tracker_db_manager_shutdown (void)
{
	guint i;

	if (!initialized) {
		return;
	}

	for (i = 1; i < G_N_ELEMENTS (dbs); i++) {
		if (dbs[i].abs_filename) {
			g_free (dbs[i].abs_filename);
			dbs[i].abs_filename = NULL;

			if (dbs[i].iface) {
				g_object_unref (dbs[i].iface);
				dbs[i].iface = NULL;
			}
		}
	}

	g_free (transient_filename);
	transient_filename = NULL;
	g_free (data_dir);
	data_dir = NULL;
	g_free (user_data_dir);
	user_data_dir = NULL;
	g_free (sys_tmp_dir);
	sys_tmp_dir = NULL;
	g_free (sql_dir);

	/* shutdown db interfaces in all threads */
	g_static_private_free (&interface_data_key);

	/* Since we don't reference this enum anywhere, we do
	 * it here to make sure it exists when we call
	 * g_type_class_peek(). This wouldn't be necessary if
	 * it was a param in a GObject for example.
	 *
	 * This does mean that we are leaking by 1 reference
	 * here and should clean it up, but it doesn't grow so
	 * this is acceptable.
	 */
	g_type_class_unref (db_type_enum_class_pointer);
	db_type_enum_class_pointer = NULL;

	initialized = FALSE;
	locations_initialized = FALSE;

	if ((tracker_db_manager_get_flags (NULL, NULL) & TRACKER_DB_MANAGER_READONLY) == 0) {
		/* do not delete in-use file for read-only mode (direct access) */
		g_unlink (in_use_filename);
	}

	g_free (in_use_filename);
	in_use_filename = NULL;
}

void
tracker_db_manager_remove_all (gboolean rm_journal)
{
	g_return_if_fail (initialized != FALSE);

	db_manager_remove_all (rm_journal);
}


void
tracker_db_manager_move_to_temp (void)
{
	guint i;
	gchar *cpath, *filename;
	gchar *fullpath;
	gchar *directory, *rotate_to = NULL;
	const gchar *dirs[3] = { NULL, NULL, NULL };
	gsize chunk_size = 0;
	gboolean do_rotate = FALSE;

	g_return_if_fail (initialized != FALSE);

	g_message ("Moving all database files");

	for (i = 1; i < G_N_ELEMENTS (dbs); i++) {
		filename = g_strdup_printf ("%s.tmp", dbs[i].abs_filename);
		g_message ("  Renaming database:'%s' -> '%s'",
		           dbs[i].abs_filename, filename);
		if (g_rename (dbs[i].abs_filename, filename) == -1) {
			g_critical ("%s", g_strerror (errno));
		}
		g_free (filename);
	}

	cpath = g_strdup (tracker_db_journal_get_filename ());

	directory = g_path_get_dirname (cpath);

	tracker_db_journal_get_rotating (&do_rotate, &chunk_size, &rotate_to);

	dirs[0] = directory;
	dirs[1] = do_rotate ? rotate_to : NULL;

	for (i = 0; dirs[i] != NULL; i++) {
		GDir *journal_dir;
		const gchar *f_name;

		journal_dir = g_dir_open (dirs[i], 0, NULL);
		f_name = g_dir_read_name (journal_dir);

		while (f_name) {
			if (f_name) {
				if (!g_str_has_prefix (f_name, TRACKER_DB_JOURNAL_FILENAME ".")) {
					f_name = g_dir_read_name (journal_dir);
					continue;
				}
			}

			fullpath = g_build_filename (dirs[i], f_name, NULL);
			filename = g_strdup_printf ("%s.tmp", fullpath);
			if (g_rename (fullpath, filename) == -1) {
				g_critical ("%s", g_strerror (errno));
			}
			g_free (filename);
			g_free (fullpath);
			f_name = g_dir_read_name (journal_dir);
		}

		g_dir_close (journal_dir);
	}

	fullpath = g_build_filename (directory, TRACKER_DB_JOURNAL_ONTOLOGY_FILENAME, NULL);
	filename = g_strdup_printf ("%s.tmp", fullpath);
	if (g_rename (fullpath, filename) == -1) {
		g_critical ("%s", g_strerror (errno));
	}
	g_free (filename);
	g_free (fullpath);

	g_free (rotate_to);
	g_free (directory);

	filename = g_strdup_printf ("%s.tmp", cpath);
	g_message ("  Renaming journal:'%s' -> '%s'",
	           cpath, filename);
	if (g_rename (cpath, filename) == -1) {
		g_critical ("%s", g_strerror (errno));
	}
	g_free (cpath);
	g_free (filename);
}


void
tracker_db_manager_restore_from_temp (void)
{
	guint i;
	gchar *cpath, *filename;
	gchar *fullpath;
	gchar *directory, *rotate_to = NULL;
	const gchar *dirs[3] = { NULL, NULL, NULL };
	gsize chunk_size = 0;
	gboolean do_rotate = FALSE;

	g_return_if_fail (locations_initialized != FALSE);

	g_message ("Moving all database files");

	for (i = 1; i < G_N_ELEMENTS (dbs); i++) {
		filename = g_strdup_printf ("%s.tmp", dbs[i].abs_filename);
		g_message ("  Renaming database:'%s' -> '%s'",
		           dbs[i].abs_filename, filename);
		if (g_rename (dbs[i].abs_filename, filename) == -1) {
			g_critical ("%s", g_strerror (errno));
		}
		g_free (filename);
	}

	cpath = g_strdup (tracker_db_journal_get_filename ());
	filename = g_strdup_printf ("%s.tmp", cpath);
	g_message ("  Renaming journal:'%s' -> '%s'",
	           filename, cpath);
	if (g_rename (filename, cpath) == -1) {
		g_critical ("%s", g_strerror (errno));
	}
	g_free (filename);

	directory = g_path_get_dirname (cpath);
	tracker_db_journal_get_rotating (&do_rotate, &chunk_size, &rotate_to);

	fullpath = g_build_filename (directory, TRACKER_DB_JOURNAL_ONTOLOGY_FILENAME, NULL);
	filename = g_strdup_printf ("%s.tmp", fullpath);
	if (g_rename (filename, fullpath) == -1) {
		g_critical ("%s", g_strerror (errno));
	}
	g_free (filename);
	g_free (fullpath);

	dirs[0] = directory;
	dirs[1] = do_rotate ? rotate_to : NULL;

	for (i = 0; dirs[i] != NULL; i++) {
		GDir *journal_dir;
		const gchar *f_name;

		journal_dir = g_dir_open (dirs[i], 0, NULL);
		f_name = g_dir_read_name (journal_dir);

		while (f_name) {
			gchar *ptr;

			if (f_name) {
				if (!g_str_has_suffix (f_name, ".tmp")) {
					f_name = g_dir_read_name (journal_dir);
					continue;
				}
			}

			fullpath = g_build_filename (dirs[i], f_name, NULL);
			filename = g_strdup (fullpath);
			ptr = strstr (filename, ".tmp");
			if (ptr) {
				*ptr = '\0';
				if (g_rename (fullpath, filename) == -1) {
					g_critical ("%s", g_strerror (errno));
				}
			}
			g_free (filename);
			g_free (fullpath);
			f_name = g_dir_read_name (journal_dir);
		}
		g_dir_close (journal_dir);
	}

	g_free (rotate_to);
	g_free (directory);
	g_free (cpath);
}

void
tracker_db_manager_remove_temp (void)
{
	guint i;
	gchar *cpath, *filename;
	gchar *directory, *rotate_to = NULL;
	gsize chunk_size = 0;
	gboolean do_rotate = FALSE;
	const gchar *dirs[3] = { NULL, NULL, NULL };

	g_return_if_fail (locations_initialized != FALSE);

	g_message ("Removing all temp database files");

	for (i = 1; i < G_N_ELEMENTS (dbs); i++) {
		filename = g_strdup_printf ("%s.tmp", dbs[i].abs_filename);
		g_message ("  Removing temp database:'%s'",
		           filename);
		if (g_unlink (filename) == -1) {
			g_message ("%s", g_strerror (errno));
		}
		g_free (filename);
	}

	cpath = g_strdup (tracker_db_journal_get_filename ());
	filename = g_strdup_printf ("%s.tmp", cpath);
	g_message ("  Removing temp journal:'%s'",
	           filename);
	if (g_unlink (filename) == -1) {
		g_message ("%s", g_strerror (errno));
	}
	g_free (filename);

	directory = g_path_get_dirname (cpath);
	tracker_db_journal_get_rotating (&do_rotate, &chunk_size, &rotate_to);
	g_free (cpath);

	cpath = g_build_filename (directory, TRACKER_DB_JOURNAL_ONTOLOGY_FILENAME, NULL);
	filename = g_strdup_printf ("%s.tmp", cpath);
	if (g_unlink (filename) == -1) {
		g_message ("%s", g_strerror (errno));
	}
	g_free (filename);
	g_free (cpath);

	dirs[0] = directory;
	dirs[1] = do_rotate ? rotate_to : NULL;

	for (i = 0; dirs[i] != NULL; i++) {
		GDir *journal_dir;
		const gchar *f_name;

		journal_dir = g_dir_open (dirs[i], 0, NULL);
		f_name = g_dir_read_name (journal_dir);

		while (f_name) {
			if (f_name) {
				if (!g_str_has_suffix (f_name, ".tmp")) {
					f_name = g_dir_read_name (journal_dir);
					continue;
				}
			}

			filename = g_build_filename (dirs[i], f_name, NULL);
			if (g_unlink (filename) == -1) {
				g_message ("%s", g_strerror (errno));
			}
			g_free (filename);

			f_name = g_dir_read_name (journal_dir);
		}

		g_dir_close (journal_dir);
	}

	g_free (rotate_to);
	g_free (directory);
}

void
tracker_db_manager_optimize (void)
{
	gboolean dbs_are_open = FALSE;
	TrackerDBInterface *iface;

	g_return_if_fail (initialized != FALSE);

	g_message ("Optimizing database...");

	g_message ("  Checking database is not in use");

	iface = tracker_db_manager_get_db_interface ();

	/* Check if any connections are open? */
	if (G_OBJECT (iface)->ref_count > 1) {
		g_message ("  database is still in use with %d references!",
		           G_OBJECT (iface)->ref_count);

		dbs_are_open = TRUE;
	}

	if (dbs_are_open) {
		g_message ("  Not optimizing database, still in use with > 1 reference");
		return;
	}

	/* Optimize the metadata database */
	db_manager_analyze (TRACKER_DB_METADATA, iface);
}

const gchar *
tracker_db_manager_get_file (TrackerDB db)
{
	g_return_val_if_fail (initialized != FALSE, NULL);

	return dbs[db].abs_filename;
}

/**
 * tracker_db_manager_get_db_interfaces:
 * @num: amount of TrackerDB files wanted
 * @...: All the files that you want in the connection as TrackerDB items
 *
 * Request a database connection where the first requested file gets connected
 * to and the subsequent requsted files get attached to the connection.
 *
 * The caller must g_object_unref the result when finished using it.
 *
 * returns: (caller-owns): a database connection
 **/
static TrackerDBInterface *
tracker_db_manager_get_db_interfaces (gint num, ...)
{
	gint                n_args;
	va_list                     args;
	TrackerDBInterface *connection = NULL;

	g_return_val_if_fail (initialized != FALSE, NULL);

	va_start (args, num);
	for (n_args = 1; n_args <= num; n_args++) {
		TrackerDB db = va_arg (args, TrackerDB);

		if (!connection) {
			connection = tracker_db_interface_sqlite_new (dbs[db].abs_filename,
			                                              transient_filename);
			db_set_params (connection,
			               dbs[db].cache_size,
			               dbs[db].page_size);

			db_exec_no_reply (connection,
			                  "ATTACH '%s' as 'transient'",
			                  transient_filename);

		} else {
			db_exec_no_reply (connection,
			                  "ATTACH '%s' as '%s'",
			                  dbs[db].abs_filename,
			                  dbs[db].name);
		}

	}
	va_end (args);

	return connection;
}

static TrackerDBInterface *
tracker_db_manager_get_db_interfaces_ro (gint num, ...)
{
	gint                n_args;
	va_list             args;
	TrackerDBInterface *connection = NULL;

	g_return_val_if_fail (initialized != FALSE, NULL);

	va_start (args, num);
	for (n_args = 1; n_args <= num; n_args++) {
		TrackerDB db = va_arg (args, TrackerDB);

		if (!connection) {
			connection = tracker_db_interface_sqlite_new_ro (dbs[db].abs_filename,
			                                                 transient_filename);
			db_set_params (connection,
			               dbs[db].cache_size,
			               dbs[db].page_size);

			db_exec_no_reply (connection,
			                  "ATTACH '%s' as 'transient'",
			                  transient_filename);

		} else {
			db_exec_no_reply (connection,
			                  "ATTACH '%s' as '%s'",
			                  dbs[db].abs_filename,
			                  dbs[db].name);
		}

	}
	va_end (args);

	return connection;
}


/**
 * tracker_db_manager_get_db_interface:
 * @db: the database file wanted
 *
 * Request a database connection to the database file @db.
 *
 * The caller must NOT g_object_unref the result
 *
 * returns: (callee-owns): a database connection
 **/
TrackerDBInterface *
tracker_db_manager_get_db_interface (void)
{
	TrackerDBInterface *interface;

	g_return_val_if_fail (initialized != FALSE, NULL);
	interface = g_static_private_get (&interface_data_key);

	/* Ensure the interface is there */
	if (!interface) {
		interface = tracker_db_manager_get_db_interfaces (1,
		                                                  TRACKER_DB_METADATA);

		tracker_db_interface_sqlite_fts_init (interface, FALSE);


		tracker_db_interface_set_max_stmt_cache_size (interface,
		                                              TRACKER_DB_STATEMENT_CACHE_TYPE_SELECT,
		                                              s_cache_size);

		tracker_db_interface_set_max_stmt_cache_size (interface,
		                                              TRACKER_DB_STATEMENT_CACHE_TYPE_UPDATE,
		                                              u_cache_size);

		g_static_private_set (&interface_data_key, interface, (GDestroyNotify) g_object_unref);
	}

	return interface;
}

/**
 * tracker_db_manager_has_enough_space:
 *
 * Checks whether the file system, where the database files are stored,
 * has enough free space to allow modifications.
 *
 * returns: TRUE if there is enough space, FALSE otherwise
 **/
gboolean
tracker_db_manager_has_enough_space  (void)
{
	return tracker_file_system_has_enough_space (data_dir, TRACKER_DB_MIN_REQUIRED_SPACE, FALSE);
}


inline static gchar *
get_first_index_filename (void)
{
	return g_build_filename (g_get_user_cache_dir (),
	                         "tracker",
	                         FIRST_INDEX_FILENAME,
	                         NULL);
}

/**
 * tracker_db_manager_get_first_index_done:
 *
 * Check if first full index of files was already done.
 *
 * Returns: %TRUE if a first full index have been done, %FALSE otherwise.
 **/
gboolean
tracker_db_manager_get_first_index_done (void)
{
	gboolean exists;
	gchar *filename;

	filename = get_first_index_filename ();
	exists = g_file_test (filename, G_FILE_TEST_EXISTS);
	g_free (filename);

	return exists;
}

/**
 * tracker_db_manager_set_first_index_done:
 *
 * Set the status of the first full index of files. Should be set to
 *  %FALSE if the index was never done or if a reindex is needed. When
 *  the index is completed, should be set to %TRUE.
 **/
void
tracker_db_manager_set_first_index_done (gboolean done)
{
	gboolean already_exists;
	gchar *filename;

	filename = get_first_index_filename ();
	already_exists = g_file_test (filename, G_FILE_TEST_EXISTS);

	if (done && !already_exists) {
		GError *error = NULL;

		/* If done, create stamp file if not already there */
		if (!g_file_set_contents (filename, PACKAGE_VERSION, -1, &error)) {
			g_warning ("  Could not create file:'%s' failed, %s",
			           filename,
			           error->message);
			g_error_free (error);
		} else {
			g_message ("  First index file:'%s' created",
			           filename);
		}
	} else if (!done && already_exists) {
		/* If NOT done, remove stamp file */
		g_message ("  Removing first index file:'%s'", filename);

		if (g_remove (filename)) {
			g_warning ("    Could not remove file:'%s', %s",
			           filename,
			           g_strerror (errno));
		}
	}

	g_free (filename);
}

inline static gchar *
get_last_crawl_filename (void)
{
	return g_build_filename (g_get_user_cache_dir (),
	                         "tracker",
	                         LAST_CRAWL_FILENAME,
	                         NULL);
}

/**
 * tracker_db_manager_get_last_crawl_done:
 *
 * Check when last crawl was performed.
 *
 * Returns: time_t() value when last crawl occurred, otherwise 0.
 **/
guint64
tracker_db_manager_get_last_crawl_done (void)
{
	gchar *filename;
	gchar *content;
	guint64 then;

	filename = get_last_crawl_filename ();

	if (!g_file_get_contents (filename, &content, NULL, NULL)) {
		g_message ("  No previous timestamp, crawling forced");
		return 0;
	}

	then = g_ascii_strtoull (content, NULL, 10);
	g_free (content);

	return then;
}

/**
 * tracker_db_manager_set_last_crawl_done:
 *
 * Set the status of the first full index of files. Should be set to
 *  %FALSE if the index was never done or if a reindex is needed. When
 *  the index is completed, should be set to %TRUE.
 **/
void
tracker_db_manager_set_last_crawl_done (gboolean done)
{
	gboolean already_exists;
	gchar *filename;

	filename = get_last_crawl_filename ();
	already_exists = g_file_test (filename, G_FILE_TEST_EXISTS);

	if (done && !already_exists) {
		GError *error = NULL;
		gchar *content;

		content = g_strdup_printf ("%" G_GUINT64_FORMAT, (guint64) time (NULL));

		/* If done, create stamp file if not already there */
		if (!g_file_set_contents (filename, content, -1, &error)) {
			g_warning ("  Could not create file:'%s' failed, %s",
			           filename,
			           error->message);
			g_error_free (error);
		} else {
			g_message ("  Last crawl file:'%s' created",
			           filename);
		}

		g_free (content);
	} else if (!done && already_exists) {
		/* If NOT done, remove stamp file */
		g_message ("  Removing last crawl file:'%s'", filename);

		if (g_remove (filename)) {
			g_warning ("    Could not remove file:'%s', %s",
			           filename,
			           g_strerror (errno));
		}
	}

	g_free (filename);
}

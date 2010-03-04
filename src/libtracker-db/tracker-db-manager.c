/*
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
#include <regex.h>
#include <zlib.h>
#include <locale.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdio.h>
#include <fcntl.h>
#include <math.h>

#include <glib/gstdio.h>

#include <libtracker-common/tracker-date-time.h>
#include <libtracker-common/tracker-file-utils.h>
#include <libtracker-common/tracker-ontologies.h>
#include <libtracker-common/tracker-property.h>
#include <libtracker-common/tracker-utils.h>

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
#define TRACKER_DB_VERSION_NOW        TRACKER_DB_VERSION_12
#define TRACKER_DB_VERSION_FILE       "db-version.txt"

#define IN_USE_FILENAME               ".meta.isrunning"

typedef enum {
	TRACKER_DB_LOCATION_DATA_DIR,
	TRACKER_DB_LOCATION_USER_DATA_DIR,
	TRACKER_DB_LOCATION_SYS_TMP_DIR,
} TrackerDBLocation;

typedef enum {
	TRACKER_DB_VERSION_UNKNOWN, /* Unknown */
	TRACKER_DB_VERSION_01,      /* Version 0.6.6  (before indexer-split) */
	TRACKER_DB_VERSION_02,      /* Version 0.6.90 (after  indexer-split) */
	TRACKER_DB_VERSION_03,      /* Version 0.6.91 (stable release) */
	TRACKER_DB_VERSION_04,      /* Version 0.6.92 (current TRUNK) */
	TRACKER_DB_VERSION_05,      /* Version 0.7    (vstore branch) */
	TRACKER_DB_VERSION_06,      /* Version 0.7.4  (nothing special) */
	TRACKER_DB_VERSION_07,      /* Version 0.7.12 (nmo ontology) */
	TRACKER_DB_VERSION_08,      /* Version 0.7.13 (coalesce & writeback) */
	TRACKER_DB_VERSION_09,      /* Version 0.7.17 (mlo ontology) */
	TRACKER_DB_VERSION_10,      /* Version 0.7.20 (nco im ontology) */
	TRACKER_DB_VERSION_11,       /* Version 0.7.21 (named graphs/localtime) */
	TRACKER_DB_VERSION_12       /* Version 0.7.22 (fts-limits branch) */
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
	gboolean            add_functions;
	gboolean            attached;
	gboolean            is_index;
	guint64             mtime;
} TrackerDBDefinition;

typedef struct {
	GString *string;     /* The string we are accumulating */
} AggregateData;

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
	  FALSE,
	  0 },
	{ TRACKER_DB_METADATA,
	  TRACKER_DB_LOCATION_DATA_DIR,
	  NULL,
	  "meta.db",
	  "meta",
	  NULL,
	  2000,
	  TRACKER_DB_PAGE_SIZE_DONT_SET,
	  TRUE,
	  FALSE,
	  FALSE,
	  0 },
	{ TRACKER_DB_CONTENTS,
	  TRACKER_DB_LOCATION_DATA_DIR,
	  NULL,
	  "contents.db",
	  "contents",
	  NULL,
	  1024,
	  TRACKER_DB_PAGE_SIZE_DONT_SET,
	  FALSE,
	  FALSE,
	  FALSE,
	  0 },
	{ TRACKER_DB_FULLTEXT,
	  TRACKER_DB_LOCATION_DATA_DIR,
	  NULL,
	  "fulltext.db",
	  "fulltext",
	  NULL,
	  512,
	  TRACKER_DB_PAGE_SIZE_DONT_SET,
	  TRUE,
	  FALSE,
	  TRUE,
	  0 },
};

static gboolean            db_exec_no_reply    (TrackerDBInterface *iface,
                                                const gchar        *query,
                                                ...);
static TrackerDBInterface *db_interface_create (TrackerDB           db);
static TrackerDBInterface *tracker_db_manager_get_db_interfaces     (gint num, ...);
static TrackerDBInterface *tracker_db_manager_get_db_interfaces_ro  (gint num, ...);

static gboolean              initialized;
static gboolean              locations_initialized;
static gchar                *sql_dir;
static gchar                *data_dir = NULL;
static gchar                *user_data_dir = NULL;
static gchar                *sys_tmp_dir = NULL;
static gpointer              db_type_enum_class_pointer;
static TrackerDBInterface   *resources_iface;
static TrackerDBManagerFlags old_flags = 0;

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

static void
load_sql_file (TrackerDBInterface *iface,
               const gchar        *file,
               const gchar        *delimiter)
{
	gchar *path, *content, **queries;
	gint   count;
	gint   i;

	path = g_build_filename (sql_dir, file, NULL);

	if (!delimiter) {
		delimiter = ";";
	}

	if (!g_file_get_contents (path, &content, NULL, NULL)) {
		g_critical ("Cannot read SQL file:'%s', please reinstall tracker"
		            " or check read permissions on the file if it exists", path);
		g_assert_not_reached ();
	}

	queries = g_strsplit (content, delimiter, -1);

	for (i = 0, count = 0; queries[i]; i++) {
		GError *error = NULL;
		gchar  *sql;

		/* Skip white space, including control characters */
		for (sql = queries[i]; sql && g_ascii_isspace (sql[0]); sql++);

		if (!sql || sql[0] == '\0') {
			continue;
		}

		tracker_db_interface_execute_query (iface, &error, "%s", sql);

		if (error) {
			g_warning ("Error loading query:'%s' #%d, %s", file, i, error->message);
			g_error_free (error);
			continue;
		}

		count++;
	}

	g_message ("  Loaded SQL file:'%s' (%d queries)", file, count);

	g_strfreev (queries);
	g_free (content);
	g_free (path);
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

/* Converts date/time in UTC format to ISO 8160 standardised format for display */
static GValue
function_date_to_str (TrackerDBInterface *interface,
                      gint                argc,
                      GValue              values[])
{
	GValue  result = { 0, };
	gchar  *str;

	str = tracker_date_to_string (g_value_get_double (&values[0]));
	g_value_init (&result, G_TYPE_STRING);
	g_value_take_string (&result, str);

	return result;
}

static GValue
function_regexp (TrackerDBInterface *interface,
                 gint                argc,
                 GValue                      values[])
{
	GValue  result = { 0, };
	regex_t         regex;
	int     ret;

	if (argc != 2) {
		g_critical ("Invalid argument count");
		return result;
	}

	ret = regcomp (&regex,
	               g_value_get_string (&values[0]),
	               REG_EXTENDED | REG_NOSUB);

	if (ret != 0) {
		g_critical ("Error compiling regular expression");
		return result;
	}

	ret = regexec (&regex,
	               g_value_get_string (&values[1]),
	               0, NULL, 0);

	g_value_init (&result, G_TYPE_INT);
	g_value_set_int (&result, (ret == REG_NOMATCH) ? 0 : 1);
	regfree (&regex);

	return result;
}

TrackerDBManagerFlags
tracker_db_manager_get_flags (void)
{
	return old_flags;
}

static GValue
function_sparql_string_join (TrackerDBInterface *interface,
			     gint                argc,
			     GValue              values[])
{
	GValue result = { 0, };
	GString *str = NULL;
	const gchar *separator;
	gint i;

	/* fn:string-join (str1, str2, ..., separator) */

	if (!G_VALUE_HOLDS_STRING (&values[argc-1])) {
		/* Fail */
		return result;
	}

	separator = g_value_get_string (&values[argc-1]);

	for (i = 0;i < argc-1; i++) {
		if (G_VALUE_HOLDS_STRING (&values[i])) {
			if (!str) {
				str = g_string_new (g_value_get_string (&values[i]));
			} else {
				g_string_append_printf (str, "%s%s",
							separator,
							g_value_get_string (&values[i]));
			}
		}
	}

	if (str) {
		g_value_init (&result, G_TYPE_STRING);
		g_value_set_string (&result, str->str);

		g_string_free (str, TRUE);
	}

	return result;
}

/* Create a title-type string from the filename for replacing missing ones */
static GValue
function_sparql_string_from_filename (TrackerDBInterface *interface,
				      gint                argc,
				      GValue              values[])
{
	GValue  result = { 0, };
	gchar  *name = NULL;
	gchar  *suffix = NULL;

	if (argc != 1) {
		g_critical ("Invalid argument count");
		return result;
	}

	/* "/home/user/path/title_of_the_movie.movie" -> "title of the movie"
	   Only for local files currently, do we need to change? */

	name = g_filename_display_basename (g_value_get_string (&values[0]));
	if (!name) {
		return result;
	}

	suffix = g_strrstr (name, ".");

	if (suffix) {
		*suffix = '\0';
	}

	g_strdelimit (name, "._", ' ');

	g_value_init (&result, G_TYPE_STRING);
	g_value_set_string (&result, name);

	g_free (name);

	return result;
}

static GValue
function_sparql_cartesian_distance (TrackerDBInterface *interface,
				    gint                argc,
				    GValue              values[])
{
	GValue result = { 0, };
	gdouble lat1;
	gdouble lat2;
	gdouble lon1;
	gdouble lon2;

	gdouble R;
	gdouble a;
	gdouble b;
	gdouble c;
	gdouble d;

	if (argc != 4) {
		g_critical ("Invalid argument count");
		return result;
	}

	lat1 = g_value_get_double (&values[0])*M_PI/180;
	lat2 = g_value_get_double (&values[1])*M_PI/180;
	lon1 = g_value_get_double (&values[2])*M_PI/180;
	lon2 = g_value_get_double (&values[3])*M_PI/180;

	R = 6371000;
	a = M_PI/2 - lat1;
	b = M_PI/2 - lat2;
	c = sqrt(a*a + b*b - 2*a*b*cos(lon2 - lon1));
	d = R*c;

	g_value_init (&result, G_TYPE_DOUBLE);
	g_value_set_double (&result, d);

	return result;
}

static GValue
function_sparql_haversine_distance (TrackerDBInterface *interface,
				    gint                argc,
				    GValue              values[])
{
	GValue result = { 0, };
	gdouble lat1;
	gdouble lat2;
	gdouble lon1;
	gdouble lon2;

	gdouble R;
	gdouble dLat;
	gdouble dLon;
	gdouble a;
	gdouble c;
	gdouble d;

	if (argc != 4) {
		g_critical ("Invalid argument count");
		return result;
	}

	lat1 = g_value_get_double (&values[0])*M_PI/180;
	lat2 = g_value_get_double (&values[1])*M_PI/180;
	lon1 = g_value_get_double (&values[2])*M_PI/180;
	lon2 = g_value_get_double (&values[3])*M_PI/180;

	R = 6371000;
	dLat = (lat2-lat1);
	dLon = (lon2-lon1);
	a = sin(dLat/2) * sin(dLat/2) + cos(lat1) * cos(lat2) *  sin(dLon/2) * sin(dLon/2);
	c = 2 * atan2(sqrt(a), sqrt(1-a));
	d = R * c;

	g_value_init (&result, G_TYPE_DOUBLE);
	g_value_set_double (&result, d);

	return result;
}

static void
function_group_concat_step (TrackerDBInterface *interface,
                            void               *aggregate_context,
                            gint                argc,
                            GValue              values[])
{
	AggregateData *p;

	g_return_if_fail (argc == 1);

	p = aggregate_context;

	if (!p->string) {
		p->string = g_string_new ("");
	} else {
		p->string = g_string_append (p->string, "|");
	}

	if (G_VALUE_HOLDS_STRING (&values[0])) {
		p->string = g_string_append (p->string, g_value_get_string (&values[0]));
	}
}

static GValue
function_group_concat_final (TrackerDBInterface *interface,
                             void               *aggregate_context)
{
	GValue result = { 0, };
	AggregateData *p;

	p = aggregate_context;

	g_value_init (&result, G_TYPE_STRING);
	g_value_set_string (&result, p->string->str);

	g_string_free (p->string, TRUE);

	return result;
}

static GValue
function_sparql_regex (TrackerDBInterface *interface,
                       gint                  argc,
                       GValue                values[])
{
	GValue  result = { 0, };
	gboolean        ret;
	const gchar *text, *pattern, *flags;
	GRegexCompileFlags regex_flags;

	if (argc != 3) {
		g_critical ("Invalid argument count");
		return result;
	}

	text = g_value_get_string (&values[0]);
	pattern = g_value_get_string (&values[1]);
	flags = g_value_get_string (&values[2]);

	regex_flags = 0;
	while (*flags) {
		switch (*flags) {
		case 's':
			regex_flags |= G_REGEX_DOTALL;
			break;
		case 'm':
			regex_flags |= G_REGEX_MULTILINE;
			break;
		case 'i':
			regex_flags |= G_REGEX_CASELESS;
			break;
		case 'x':
			regex_flags |= G_REGEX_EXTENDED;
			break;
		default:
			g_critical ("Invalid SPARQL regex flag '%c'", *flags);
			return result;
		}
		flags++;
	}

	ret = g_regex_match_simple (pattern, text, regex_flags, 0);

	g_value_init (&result, G_TYPE_INT);
	g_value_set_int (&result, ret);

	return result;
}

static gchar *
function_uncompress_string (const gchar *ptr,
                            gint         size,
                            gint        *uncompressed_size)
{
	z_stream       zs;
	gchar         *buf, *swap;
	unsigned char  obuf[ZLIB_BUF_SIZE];
	gint           rv, asiz, bsiz, osiz;

	zs.zalloc = Z_NULL;
	zs.zfree = Z_NULL;
	zs.opaque = Z_NULL;

	if (inflateInit2 (&zs, 15) != Z_OK) {
		return NULL;
	}

	asiz = size * 2 + 16;

	if (asiz < ZLIB_BUF_SIZE) {
		asiz = ZLIB_BUF_SIZE;
	}

	if (!(buf = malloc (asiz))) {
		inflateEnd (&zs);
		return NULL;
	}

	bsiz = 0;
	zs.next_in = (unsigned char *)ptr;
	zs.avail_in = size;
	zs.next_out = obuf;
	zs.avail_out = ZLIB_BUF_SIZE;

	while ((rv = inflate (&zs, Z_NO_FLUSH)) == Z_OK) {
		osiz = ZLIB_BUF_SIZE - zs.avail_out;

		if (bsiz + osiz >= asiz) {
			asiz = asiz * 2 + osiz;

			if (!(swap = realloc (buf, asiz))) {
				free (buf);
				inflateEnd (&zs);
				return NULL;
			}

			buf = swap;
		}

		memcpy (buf + bsiz, obuf, osiz);
		bsiz += osiz;
		zs.next_out = obuf;
		zs.avail_out = ZLIB_BUF_SIZE;
	}

	if (rv != Z_STREAM_END) {
		free (buf);
		inflateEnd (&zs);
		return NULL;
	}
	osiz = ZLIB_BUF_SIZE - zs.avail_out;

	if (bsiz + osiz >= asiz) {
		asiz = asiz * 2 + osiz;

		if (!(swap = realloc (buf, asiz))) {
			free (buf);
			inflateEnd (&zs);
			return NULL;
		}

		buf = swap;
	}

	memcpy (buf + bsiz, obuf, osiz);
	bsiz += osiz;
	buf[bsiz] = '\0';
	*uncompressed_size = bsiz;
	inflateEnd (&zs);

	return buf;
}

static GByteArray *
function_compress_string (const gchar *text)
{
	GByteArray *array;
	z_stream zs;
	gchar *buf, *swap;
	guchar obuf[ZLIB_BUF_SIZE];
	gint rv, asiz, bsiz, osiz, size;

	size = strlen (text);

	zs.zalloc = Z_NULL;
	zs.zfree = Z_NULL;
	zs.opaque = Z_NULL;

	if (deflateInit2 (&zs, 6, Z_DEFLATED, 15, 6, Z_DEFAULT_STRATEGY) != Z_OK) {
		return NULL;
	}

	asiz = size + 16;

	if (asiz < ZLIB_BUF_SIZE) {
		asiz = ZLIB_BUF_SIZE;
	}

	if (!(buf = malloc (asiz))) {
		deflateEnd (&zs);
		return NULL;
	}

	bsiz = 0;
	zs.next_in = (unsigned char *) text;
	zs.avail_in = size;
	zs.next_out = obuf;
	zs.avail_out = ZLIB_BUF_SIZE;

	while ((rv = deflate (&zs, Z_FINISH)) == Z_OK) {
		osiz = ZLIB_BUF_SIZE - zs.avail_out;

		if (bsiz + osiz > asiz) {
			asiz = asiz * 2 + osiz;

			if (!(swap = realloc (buf, asiz))) {
				free (buf);
				deflateEnd (&zs);
				return NULL;
			}

			buf = swap;
		}

		memcpy (buf + bsiz, obuf, osiz);
		bsiz += osiz;
		zs.next_out = obuf;
		zs.avail_out = ZLIB_BUF_SIZE;
	}

	if (rv != Z_STREAM_END) {
		free (buf);
		deflateEnd (&zs);
		return NULL;
	}

	osiz = ZLIB_BUF_SIZE - zs.avail_out;

	if (bsiz + osiz + 1 > asiz) {
		asiz = asiz * 2 + osiz;

		if (!(swap = realloc (buf, asiz))) {
			free (buf);
			deflateEnd (&zs);
			return NULL;
		}

		buf = swap;
	}

	memcpy (buf + bsiz, obuf, osiz);
	bsiz += osiz;
	buf[bsiz] = '\0';

	array = g_byte_array_new ();
	g_byte_array_append (array, (const guint8 *) buf, bsiz);

	g_free (buf);

	deflateEnd (&zs);

	return array;
}

static GValue
function_uncompress (TrackerDBInterface *interface,
                     gint                argc,
                     GValue              values[])
{
	GByteArray *array;
	GValue      result = { 0, };
	gchar      *output;
	gint        len;

	array = g_value_get_boxed (&values[0]);

	if (!array) {
		return result;
	}

	output = function_uncompress_string ((const gchar *) array->data,
	                                     array->len,
	                                     &len);

	if (!output) {
		g_warning ("Uncompress failed");
		return result;
	}

	g_value_init (&result, G_TYPE_STRING);
	g_value_take_string (&result, output);

	return result;
}

static GValue
function_compress (TrackerDBInterface *interface,
                   gint                        argc,
                   GValue              values[])
{
	GByteArray *array;
	GValue result = { 0, };
	const gchar *text;

	text = g_value_get_string (&values[0]);

	array = function_compress_string (text);

	if (!array) {
		g_warning ("Compress failed");
		return result;
	}

	g_value_init (&result, TRACKER_TYPE_DB_BLOB);
	g_value_take_boxed (&result, array);

	return result;
}

static GValue
function_replace (TrackerDBInterface *interface,
                  gint                argc,
                  GValue              values[])
{
	GValue result = { 0, };
	gchar *str;

	str = tracker_string_replace (g_value_get_string (&values[0]),
	                              g_value_get_string (&values[1]),
	                              g_value_get_string (&values[2]));

	g_value_init (&result, G_TYPE_STRING);
	g_value_take_string (&result, str);

	return result;
}

static GValue
function_collate_key (TrackerDBInterface *interface,
                      gint                argc,
                      GValue              values[])
{
	GValue result = { 0 };
	gchar *collate_key;

	collate_key = g_utf8_collate_key (g_value_get_string (&values[0]), -1);

	g_value_init (&result, G_TYPE_STRING);
	g_value_take_string (&result, collate_key);

	return result;
}

static void
db_set_params (TrackerDBInterface *iface,
               gint                cache_size,
               gint                page_size,
               gboolean                    add_functions)
{
	tracker_db_interface_execute_query (iface, NULL, "PRAGMA synchronous = OFF;");
	tracker_db_interface_execute_query (iface, NULL, "PRAGMA count_changes = 0;");
	tracker_db_interface_execute_query (iface, NULL, "PRAGMA temp_store = FILE;");
	tracker_db_interface_execute_query (iface, NULL, "PRAGMA encoding = \"UTF-8\"");
	tracker_db_interface_execute_query (iface, NULL, "PRAGMA auto_vacuum = 0;");

	if (page_size != TRACKER_DB_PAGE_SIZE_DONT_SET) {
		g_message ("  Setting page size to %d", page_size);
		tracker_db_interface_execute_query (iface, NULL, "PRAGMA page_size = %d", page_size);
	}

	tracker_db_interface_execute_query (iface, NULL, "PRAGMA cache_size = %d", cache_size);
	g_message ("  Setting cache size to %d", cache_size);

	if (add_functions) {
		g_message ("  Adding functions (FormatDate, etc)");

		/* Create user defined functions that can be used in sql */
		tracker_db_interface_sqlite_create_function (iface,
		                                             "FormatDate",
		                                             function_date_to_str,
		                                             1);
		tracker_db_interface_sqlite_create_function (iface,
		                                             "REGEXP",
		                                             function_regexp,
		                                             2);
		tracker_db_interface_sqlite_create_function (iface,
		                                             "SparqlRegex",
		                                             function_sparql_regex,
		                                             3);
		tracker_db_interface_sqlite_create_function (iface,
		                                             "SparqlStringJoin",
		                                             function_sparql_string_join,
		                                             -1);
		tracker_db_interface_sqlite_create_function (iface,
		                                             "SparqlStringFromFilename",
		                                             function_sparql_string_from_filename,
		                                             1);
		tracker_db_interface_sqlite_create_function (iface,
		                                             "SparqlCartesianDistance",
		                                             function_sparql_cartesian_distance,
		                                             4);
		tracker_db_interface_sqlite_create_function (iface,
		                                             "SparqlHaversineDistance",
		                                             function_sparql_haversine_distance,
		                                             4);
		tracker_db_interface_sqlite_create_function (iface,
		                                             "uncompress",
		                                             function_uncompress,
		                                             1);
		tracker_db_interface_sqlite_create_function (iface,
		                                             "compress",
		                                             function_compress,
		                                             1);
		tracker_db_interface_sqlite_create_function (iface,
		                                             "replace",
		                                             function_replace,
		                                             3);

		tracker_db_interface_sqlite_create_aggregate (iface,
		                                              "group_concat",
		                                              function_group_concat_step,
		                                              1,
		                                              function_group_concat_final,
		                                              sizeof(AggregateData));

		tracker_db_interface_sqlite_create_function (iface,
		                                             "CollateKey",
		                                             function_collate_key,
		                                             1);
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

	iface = tracker_db_interface_sqlite_new (path);

	db_set_params (iface,
	               dbs[type].cache_size,
	               dbs[type].page_size,
	               dbs[type].add_functions);

	return iface;
}

static TrackerDBInterface *
db_interface_get_fulltext (void)
{
	TrackerDBInterface *iface;
	gboolean            create;

	iface = db_interface_get (TRACKER_DB_FULLTEXT, &create);

	return iface;
}

static TrackerDBInterface *
db_interface_get_contents (void)
{
	TrackerDBInterface *iface;
	gboolean            create;

	iface = db_interface_get (TRACKER_DB_CONTENTS, &create);

	if (create) {
		tracker_db_interface_start_transaction (iface);
		load_sql_file (iface, "sqlite-contents.sql", NULL);
		tracker_db_interface_end_transaction (iface);
	}

	tracker_db_interface_sqlite_create_function (iface,
	                                             "uncompress",
	                                             function_uncompress,
	                                             1);
	tracker_db_interface_sqlite_create_function (iface,
	                                             "compress",
	                                             function_compress,
	                                             1);

	return iface;
}



static TrackerDBInterface *
db_interface_get_metadata (void)
{
	TrackerDBInterface *iface;
	gboolean            create;

	iface = db_interface_get (TRACKER_DB_METADATA, &create);

	if (create) {
		tracker_db_interface_start_transaction (iface);

		/* Create tables */
		load_sql_file (iface, "sqlite-tracker.sql", NULL);

		tracker_db_interface_end_transaction (iface);
	}

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

	case TRACKER_DB_FULLTEXT:
		return db_interface_get_fulltext ();

	case TRACKER_DB_CONTENTS:
		return db_interface_get_contents ();

	default:
		g_critical ("This TrackerDB type:%d->'%s' has no interface set up yet!!",
		            db,
		            db_type_to_string (db));
		return NULL;
	}
}

static void
db_manager_remove_all (gboolean rm_journal)
{
	guint i;

	g_message ("Removing all database files");

	/* NOTE: We don't have to be initialized for this so we
	 * calculate the absolute directories here.
	 */
	for (i = 1; i < G_N_ELEMENTS (dbs); i++) {

		g_message ("  Removing database:'%s'",
		           dbs[i].abs_filename);
		g_unlink (dbs[i].abs_filename);
	}

	if (rm_journal) {
		const gchar *opath = tracker_db_journal_get_filename ();

		if (opath) {
			GFile *file;
			gchar *cpath;

			cpath = g_strdup (opath);
			tracker_db_journal_shutdown ();
			g_message ("  Removing journal:'%s'",
					   cpath);
			file = g_file_new_for_path (cpath);
			g_file_delete (file, NULL, NULL);
			g_object_unref (file);
			g_free (cpath);
		}
	}
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
db_manager_analyze (TrackerDB db)
{
	guint64             current_mtime;

	current_mtime = tracker_file_get_mtime (dbs[db].abs_filename);

	if (current_mtime > dbs[db].mtime) {
		g_message ("  Analyzing DB:'%s'", dbs[db].name);
		db_exec_no_reply (dbs[db].iface, "ANALYZE %s.Services", dbs[db].name);

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
			{ TRACKER_DB_CONTENTS,
			  "TRACKER_DB_CONTENTS",
			  "contents" },
			{ 0, NULL, NULL }
		};

		etype = g_enum_register_static ("TrackerDB", values);
	}

	return etype;
}

static void
tracker_db_manager_ensure_locale (void)
{
	TrackerDBInterface *common;
	TrackerDBStatement *stmt;
	TrackerDBResultSet *result_set;
	const gchar *current_locale;
	gchar *stored_locale = NULL;

	current_locale = setlocale (LC_COLLATE, NULL);

	common = dbs[TRACKER_DB_METADATA].iface;

	stmt = tracker_db_interface_create_statement (common, "SELECT OptionValue FROM Options WHERE OptionKey = 'CollationLocale'");
	result_set = tracker_db_statement_execute (stmt, NULL);
	g_object_unref (stmt);

	if (result_set) {
		tracker_db_result_set_get (result_set, 0, &stored_locale, -1);
		g_object_unref (result_set);
	}

	if (g_strcmp0 (current_locale, stored_locale) != 0) {
		/* Locales differ, update collate keys */
		g_message ("Updating DB locale dependent data to: %s\n", current_locale);

		stmt = tracker_db_interface_create_statement (common, "UPDATE Options SET OptionValue = ? WHERE OptionKey = 'CollationLocale'");
		tracker_db_statement_bind_text (stmt, 0, current_locale);
		tracker_db_statement_execute (stmt, NULL);
		g_object_unref (stmt);
	}

	g_free (stored_locale);
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

	/* In cases where we re-init this module, make sure
	 * we have cleaned up the ontology before we load all
	 * new databases.
	 */
	tracker_ontologies_shutdown ();

	/* Make sure we initialize all other modules we depend on */
	tracker_ontologies_init ();

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

		if (old_flags & TRACKER_DB_MANAGER_LOW_MEMORY_MODE) {
			dbs[i].cache_size /= 2;
		}
	}

	locations_initialized = TRUE;
}

gboolean
tracker_db_manager_init (TrackerDBManagerFlags  flags,
                         gboolean              *first_time,
                         gboolean               shared_cache)
{
	GType               etype;
	TrackerDBVersion    version;
	gchar              *filename;
	const gchar        *dir;
	const gchar        *env_path;
	gboolean            need_reindex;
	guint               i;
	gchar              *in_use_filename;
	int                 in_use_file;
	gboolean            loaded = FALSE;

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
	if (sys_tmp_dir)
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

	if (user_data_dir)
		g_free (user_data_dir);

	user_data_dir = g_build_filename (g_get_user_data_dir (),
	                                  "tracker",
	                                  "data",
	                                  NULL);

	if (data_dir)
		g_free (data_dir);

	data_dir = g_build_filename (g_get_user_cache_dir (),
	                             "tracker",
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
		if (dbs[i].abs_filename)
			g_free (dbs[i].abs_filename);
		dbs[i].abs_filename = g_build_filename (dir, dbs[i].file, NULL);

		if (flags & TRACKER_DB_MANAGER_LOW_MEMORY_MODE) {
			dbs[i].cache_size /= 2;
		}

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

	in_use_filename = g_build_filename (g_get_user_data_dir (),
	                                    "tracker",
	                                    "data",
	                                    IN_USE_FILENAME,
	                                    NULL);

	/* Should we reindex? If so, just remove all databases files,
	 * NOT the paths, note, that these paths are also used for
	 * other things like the nfs lock file.
	 */
	if (flags & TRACKER_DB_MANAGER_FORCE_REINDEX || need_reindex) {
		if (first_time) {
			*first_time = TRUE;
		}

		if (!tracker_file_system_has_enough_space (data_dir, TRACKER_DB_MIN_REQUIRED_SPACE, TRUE)) {
			return FALSE;
		}

		db_recreate_all ();

		/* Make sure we initialize all other modules we depend on */
		tracker_ontologies_init ();

		/* Load databases */
		g_message ("Loading databases files...");

	} else {
		gboolean must_recreate;

		/* Make sure we initialize all other modules we depend on */
		tracker_ontologies_init ();

		/* Load databases */
		g_message ("Loading databases files...");

		must_recreate = !tracker_db_journal_reader_verify_last (NULL);

		if (!must_recreate && g_file_test (in_use_filename, G_FILE_TEST_EXISTS)) {
			gsize size = 0;

			g_message ("Didn't shut down cleanly last time, doing integrity checks");

			for (i = 1; i < G_N_ELEMENTS (dbs) && !must_recreate; i++) {
				TrackerDBCursor *cursor;
				TrackerDBStatement *stmt;
				struct stat st;

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

				stmt = tracker_db_interface_create_statement (dbs[i].iface,
				                                              "PRAGMA integrity_check(1)");

				cursor = tracker_db_statement_start_cursor (stmt, NULL);
				g_object_unref (stmt);

				if (cursor) {
					if (tracker_db_cursor_iter_next (cursor)) {
						if (g_strcmp0 (tracker_db_cursor_get_string (cursor, 0), "ok") != 0) {
							must_recreate = TRUE;
						}
					}
					g_object_unref (cursor);
				}
			}
		}

		if (must_recreate) {

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
			dbs[i].iface = db_interface_create (i);
			dbs[i].mtime = tracker_file_get_mtime (dbs[i].abs_filename);
		}
	}

	in_use_file = g_open (in_use_filename,
	                      O_WRONLY | O_APPEND | O_CREAT | O_SYNC,
	                      S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
	fsync (in_use_file);
	close (in_use_file);
	g_free (in_use_filename);

	tracker_db_manager_ensure_locale ();

	initialized = TRUE;

	if (flags & TRACKER_DB_MANAGER_READONLY) {
		resources_iface = tracker_db_manager_get_db_interfaces_ro (3,
		                                                           TRACKER_DB_METADATA,
		                                                           TRACKER_DB_FULLTEXT,
		                                                           TRACKER_DB_CONTENTS);
	} else {
		resources_iface = tracker_db_manager_get_db_interfaces (3,
		                                                        TRACKER_DB_METADATA,
		                                                        TRACKER_DB_FULLTEXT,
		                                                        TRACKER_DB_CONTENTS);
	}

	return TRUE;
}

void
tracker_db_manager_shutdown (void)
{
	guint i;
	gchar *in_use_filename;

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

	g_free (data_dir);
	data_dir = NULL;
	g_free (user_data_dir);
	user_data_dir = NULL;
	g_free (sys_tmp_dir);
	sys_tmp_dir = NULL;
	g_free (sql_dir);

	if (resources_iface) {
		g_object_unref (resources_iface);
		resources_iface = NULL;
	}


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

	/* Make sure we shutdown all other modules we depend on */
	tracker_ontologies_shutdown ();

	initialized = FALSE;
	locations_initialized = FALSE;

	in_use_filename = g_build_filename (g_get_user_data_dir (),
	                                    "tracker",
	                                    "data",
	                                    IN_USE_FILENAME,
	                                    NULL);

	g_unlink (in_use_filename);

	g_free (in_use_filename);
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
	gchar *cpath, *new_filename;

	g_return_if_fail (initialized != FALSE);

	g_message ("Moving all database files");

	for (i = 1; i < G_N_ELEMENTS (dbs); i++) {
		new_filename = g_strdup_printf ("%s.tmp", dbs[i].abs_filename);
		g_message ("  Renaming database:'%s' -> '%s'",
		           dbs[i].abs_filename, new_filename);
		g_rename (dbs[i].abs_filename, new_filename);
		g_free (new_filename);
	}

	cpath = g_strdup (tracker_db_journal_get_filename ());
	new_filename = g_strdup_printf ("%s.tmp", cpath);
	g_message ("  Renaming journal:'%s' -> '%s'",
	           cpath, new_filename);
	g_rename (cpath, new_filename);
	g_free (cpath);
	g_free (new_filename);
}


void
tracker_db_manager_restore_from_temp (void)
{
	guint i;
	gchar *cpath, *new_filename;

	g_return_if_fail (locations_initialized != FALSE);

	g_message ("Moving all database files");

	for (i = 1; i < G_N_ELEMENTS (dbs); i++) {
		new_filename = g_strdup_printf ("%s.tmp", dbs[i].abs_filename);
		g_message ("  Renaming database:'%s' -> '%s'",
		           dbs[i].abs_filename, new_filename);
		g_rename (dbs[i].abs_filename, new_filename);
		g_free (new_filename);
	}

	cpath = g_strdup (tracker_db_journal_get_filename ());
	new_filename = g_strdup_printf ("%s.tmp", cpath);
	g_message ("  Renaming journal:'%s' -> '%s'",
	           cpath, new_filename);
	g_rename (cpath, new_filename);
	g_free (cpath);
	g_free (new_filename);
}

void
tracker_db_manager_remove_temp (void)
{
	guint i;
	gchar *cpath, *new_filename;

	g_return_if_fail (locations_initialized != FALSE);

	g_message ("Removing all temp database files");

	for (i = 1; i < G_N_ELEMENTS (dbs); i++) {
		new_filename = g_strdup_printf ("%s.tmp", dbs[i].abs_filename);
		g_message ("  Removing temp database:'%s'",
		           new_filename);
		g_unlink (new_filename);
		g_free (new_filename);
	}

	cpath = g_strdup (tracker_db_journal_get_filename ());
	new_filename = g_strdup_printf ("%s.tmp", cpath);
	g_message ("  Removing temp journal:'%s'",
	           new_filename);
	g_unlink (new_filename);
	g_free (cpath);
	g_free (new_filename);
}

void
tracker_db_manager_optimize (void)
{
	gboolean dbs_are_open = FALSE;
	guint    i;

	g_return_if_fail (initialized != FALSE);

	g_message ("Optimizing databases...");

	g_message ("  Checking DBs are not open");

	/* Check if any connections are open? */
	for (i = 1; i < G_N_ELEMENTS (dbs); i++) {
		if (G_OBJECT (dbs[i].iface)->ref_count > 1) {
			g_message ("  DB:'%s' is still open with %d references!",
			           dbs[i].name,
			           G_OBJECT (dbs[i].iface)->ref_count);

			dbs_are_open = TRUE;
		}
	}

	if (dbs_are_open) {
		g_message ("  Not optimizing DBs, some are still open with > 1 reference");
		return;
	}

	/* Optimize the metadata database */
	db_manager_analyze (TRACKER_DB_METADATA);
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
			connection = tracker_db_interface_sqlite_new (dbs[db].abs_filename);

			db_set_params (connection,
			               dbs[db].cache_size,
			               dbs[db].page_size,
			               TRUE);

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
	va_list                     args;
	TrackerDBInterface *connection = NULL;

	g_return_val_if_fail (initialized != FALSE, NULL);

	va_start (args, num);
	for (n_args = 1; n_args <= num; n_args++) {
		TrackerDB db = va_arg (args, TrackerDB);

		if (!connection) {
			connection = tracker_db_interface_sqlite_new_ro (dbs[db].abs_filename);
			db_set_params (connection,
			               dbs[db].cache_size,
			               dbs[db].page_size,
			               TRUE);
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
	g_return_val_if_fail (initialized != FALSE, NULL);

	return resources_iface;
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

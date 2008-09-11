/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2006, Mr Jamie McCracken (jamiemcc@gnome.org)
 * Copyright (C) 2008, Nokia

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

#include <gio/gio.h>

#include <libtracker-common/tracker-file-utils.h>
#include <libtracker-common/tracker-type-utils.h>
#include <libtracker-common/tracker-os-dependant.h>
#include <libtracker-common/tracker-ontology.h>

#include <string.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* This is temporarily disabled until hildon-thumbnailer is enabled.
 * There are performance concerns with this enabled.
 */
#undef THUMBNAIL_RETRIEVAL_ENABLED

#ifdef HAVE_HILDON_THUMBNAIL
#include <hildon-thumbnail-factory.h>
#endif

#include "tracker-metadata-utils.h"

#define METADATA_FILE_NAME_DELIMITED "File:NameDelimited"
#define METADATA_FILE_EXT            "File:Ext"
#define METADATA_FILE_PATH           "File:Path"
#define METADATA_FILE_NAME           "File:Name"
#define METADATA_FILE_LINK           "File:Link"
#define METADATA_FILE_MIMETYPE       "File:Mime"
#define METADATA_FILE_SIZE           "File:Size"
#define METADATA_FILE_MODIFIED       "File:Modified"
#define METADATA_FILE_ACCESSED       "File:Accessed"

#undef  TRY_LOCALE_TO_UTF8_CONVERSION
#define TEXT_MAX_SIZE                1048576  /* bytes */
#define TEXT_CHECK_SIZE              65535    /* bytes */

typedef struct {
	GPid pid;
	GIOChannel *stdin_channel;
	GIOChannel *stdout_channel;
	GMainLoop  *data_incoming_loop;
	gpointer data;
} ProcessContext;

static ProcessContext *metadata_context = NULL;
static void tracker_metadata_utils_get_thumbnail (const gchar *path, const gchar *mime);

static void
destroy_process_context (ProcessContext *context)
{
	g_io_channel_shutdown (context->stdin_channel, FALSE, NULL);
	g_io_channel_unref (context->stdin_channel);

	g_io_channel_shutdown (context->stdout_channel, FALSE, NULL);
	g_io_channel_unref (context->stdout_channel);

	if (g_main_loop_is_running (context->data_incoming_loop)) {
		g_main_loop_quit (context->data_incoming_loop);
	}

	g_main_loop_unref (context->data_incoming_loop);

	g_spawn_close_pid (context->pid);

	g_free (context);
}

static void
process_watch_cb (GPid     pid,
		  gint     status,
		  gpointer user_data)
{
	g_debug ("Process '%d' exited with code: %d\n", pid, status);

	if (user_data == metadata_context) {
		destroy_process_context (metadata_context);
		metadata_context = NULL;
	}
}

static ProcessContext *
create_process_context (const gchar **argv)
{
	ProcessContext *context;
	GIOChannel *stdin_channel, *stdout_channel;
	GIOFlags flags;
	GPid pid;

	if (!tracker_spawn_async_with_channels (argv, 10, &pid, &stdin_channel, &stdout_channel, NULL))
		return NULL;

	context = g_new0 (ProcessContext, 1);
	context->pid = pid;
	context->stdin_channel = stdin_channel;
	context->stdout_channel = stdout_channel;
	context->data_incoming_loop = g_main_loop_new (NULL, FALSE);

	flags = g_io_channel_get_flags (context->stdout_channel);
	flags |= G_IO_FLAG_NONBLOCK;

	g_io_channel_set_flags (context->stdout_channel, flags, NULL);

	g_child_watch_add (context->pid, process_watch_cb, context);

	return context;
}

static gboolean
tracker_metadata_read (GIOChannel   *channel,
		       GIOCondition  condition,
		       gpointer      user_data)
{
	GPtrArray *array;
	GIOStatus status = G_IO_STATUS_NORMAL;
	gchar *line;

	if (!metadata_context) {
		return FALSE;
	}

	if (condition & G_IO_IN || condition & G_IO_PRI) {
		array = metadata_context->data;

		do {
			status = g_io_channel_read_line (metadata_context->stdout_channel, &line, NULL, NULL, NULL);

			if (status == G_IO_STATUS_NORMAL && line && *line) {
				g_strstrip (line);
				g_strdelimit (line, ";", '\0');
				g_ptr_array_add (array, line);
			}
		} while (status == G_IO_STATUS_NORMAL && line && *line);

		if (status == G_IO_STATUS_EOF ||
		    status == G_IO_STATUS_ERROR ||
		    (status == G_IO_STATUS_NORMAL && !*line)) {
			/* all extractor output has been processed */
			g_main_loop_quit (metadata_context->data_incoming_loop);
			return FALSE;
		}
	}

	if (condition & G_IO_HUP || condition & G_IO_ERR) {
		return FALSE;
	}

	return TRUE;
}

static gboolean
create_metadata_context (void)
{
	const gchar *argv[2] = { LIBEXEC_PATH G_DIR_SEPARATOR_S "tracker-extract", NULL };

	if (metadata_context) {
		destroy_process_context (metadata_context);
		metadata_context = NULL;
	}

	metadata_context = create_process_context (argv);

	if (!metadata_context) {
		return FALSE;
	}

	g_io_add_watch (metadata_context->stdout_channel,
			G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP,
			tracker_metadata_read,
			metadata_context);

	return TRUE;
}

static gchar **
tracker_metadata_query_file (const gchar *path,
			     const gchar *mimetype)
{
	gchar *utf_path, *str;
	GPtrArray *array;
	GIOStatus status;
	GError *error = NULL;

	if (!path || !mimetype) {
		return NULL;
	}

	if (!metadata_context && !create_metadata_context ()) {
		return NULL;
	}

	utf_path = g_filename_from_utf8 (path, -1, NULL, NULL, NULL);

	if (!utf_path) {
		g_free (utf_path);
		return NULL;
	}

	array = g_ptr_array_sized_new (10);
	metadata_context->data = array;

	str = g_strdup_printf ("%s\n%s\n", utf_path, mimetype);
	g_free (utf_path);

	/* write path and mimetype */
	g_io_channel_write_chars (metadata_context->stdin_channel, str, -1, NULL, NULL);
	status = g_io_channel_flush (metadata_context->stdin_channel, &error);

	if (status == G_IO_STATUS_ERROR &&
	    error &&
	    g_error_matches (error, G_IO_CHANNEL_ERROR, G_IO_CHANNEL_ERROR_PIPE)) {
		/* Looks like the external extractor
		 * process has died before the child watch
		 * could handle it, try respawning.
		 */
		g_error_free (error);

		create_metadata_context ();
		metadata_context->data = array;

		g_io_channel_write_chars (metadata_context->stdin_channel, str, -1, NULL, NULL);
		status = g_io_channel_flush (metadata_context->stdin_channel, NULL);

		if (status == G_IO_STATUS_ERROR) {
			/* No point in trying again */
			g_free (str);
			return NULL;
		}
	}

	/* It will block here until all incoming
	 * metadata has been processed
	 */
	g_main_loop_run (metadata_context->data_incoming_loop);

	g_ptr_array_add (array, NULL);

	if (metadata_context) {
		metadata_context->data = NULL;
	}

	g_free (str);

	return (gchar **) g_ptr_array_free (array, FALSE);
}

static void
tracker_metadata_utils_get_embedded (const char      *path,
				     const char      *mimetype,
				     TrackerMetadata *metadata)
{
	gchar **values, *service_type;
	gint i;

	service_type = tracker_ontology_get_service_type_for_mime (mimetype);

	if (!service_type) {
		return;
	}

	if (!tracker_ontology_service_type_has_metadata (service_type)) {
		g_free (service_type);
		return;
	}

        g_free (service_type);

	values = tracker_metadata_query_file (path, mimetype);

	if (!values) {
		return;
	}

	/* parse returned values and extract keys and associated metadata */
	for (i = 0; values[i]; i++) {
		char *meta_data, *sep;
		const char *name, *value;
		char *utf_value;

		meta_data = values[i];
		sep = strchr (meta_data, '=');

		if (!sep)
			continue;

		/* zero out the separator, so we get
		 * NULL-terminated name and value
		 */
		sep[0] = '\0';
		name = meta_data;
		value = sep + 1;

		if (!name || !value)
			continue;

		if (!g_utf8_validate (value, -1, NULL)) {
			utf_value = g_locale_to_utf8 (value, -1, NULL, NULL, NULL);
		} else {
			utf_value = g_strdup (value);
		}

		if (!utf_value)
			continue;

                tracker_metadata_insert (metadata, name, utf_value);
	}

	g_strfreev (values);
}

TrackerMetadata *
tracker_metadata_utils_get_data (const gchar *path)
{
        TrackerMetadata *metadata;
	struct stat st;
	const gchar *ext;
	gchar *mimetype;

	if (g_lstat (path, &st) < 0) {
                return NULL;
        }

        metadata = tracker_metadata_new ();
	ext = strrchr (path, '.');

	if (ext) {
		tracker_metadata_insert (metadata, METADATA_FILE_EXT, g_strdup (ext + 1));
	}

	mimetype = tracker_file_get_mime_type (path);

        tracker_metadata_insert (metadata, METADATA_FILE_NAME, g_filename_display_basename (path));
	tracker_metadata_insert (metadata, METADATA_FILE_PATH, g_path_get_dirname (path));
	tracker_metadata_insert (metadata, METADATA_FILE_NAME_DELIMITED,
                                 g_filename_to_utf8 (path, -1, NULL, NULL, NULL));
	tracker_metadata_insert (metadata, METADATA_FILE_MIMETYPE, mimetype);

	if (mimetype) {

		/* FIXME: 
		 * We should determine here for which items we do and for which
		 * items we don't want to pre-create the thumbnail. */

		tracker_metadata_utils_get_thumbnail (path, mimetype);
	}

	if (S_ISLNK (st.st_mode)) {
		gchar *link_path;

		link_path = g_file_read_link (path, NULL);
		tracker_metadata_insert (metadata, METADATA_FILE_LINK,
                                         g_filename_to_utf8 (link_path, -1, NULL, NULL, NULL));
		g_free (link_path);
	}

	/* FIXME: These should be dealt directly as integer/times/whatever, not strings */
	tracker_metadata_insert (metadata, METADATA_FILE_SIZE,
                                 tracker_guint_to_string (st.st_size));
	tracker_metadata_insert (metadata, METADATA_FILE_MODIFIED,
                                 tracker_date_to_string (st.st_mtime));
	tracker_metadata_insert (metadata, METADATA_FILE_ACCESSED,
                                 tracker_date_to_string (st.st_atime));

	tracker_metadata_utils_get_embedded (path, mimetype, metadata);

        return metadata;
}

static gboolean
tracker_text_read (GIOChannel   *channel,
		   GIOCondition  condition,
		   gpointer      user_data)
{
	ProcessContext *context;
	GString *text;
	GIOStatus status;
	gchar *line;

	context = user_data;
	text = context->data;;
	status = G_IO_STATUS_NORMAL;

	if (condition & G_IO_IN || condition & G_IO_PRI) {
		do {
			status = g_io_channel_read_line (channel, &line, NULL, NULL, NULL);

			if (status == G_IO_STATUS_NORMAL) {
				g_string_append (text, line);
				g_free (line);
			}
		} while (status == G_IO_STATUS_NORMAL);

		if (status == G_IO_STATUS_EOF ||
		    status == G_IO_STATUS_ERROR) {
			g_main_loop_quit (context->data_incoming_loop);
			return FALSE;
		}
	}

	if (condition & G_IO_ERR || condition & G_IO_HUP) {
		g_main_loop_quit (context->data_incoming_loop);
		return FALSE;
	}

	return TRUE;
}

static gchar *
call_text_filter (const gchar *path,
		  const gchar *mime)
{
	ProcessContext *context;
	gchar *str, *text_filter_file;
	gchar **argv;
	GString *text;

#ifdef OS_WIN32
	str = g_strconcat (mime, "_filter.bat", NULL);
#else
	str = g_strconcat (mime, "_filter", NULL);
#endif

	text_filter_file = g_build_filename (LIBDIR,
					     "tracker",
					     "filters",
					     str,
					     NULL);

	g_free (str);

	if (!g_file_test (text_filter_file, G_FILE_TEST_EXISTS)) {
		g_free (text_filter_file);
		return NULL;
	}

	argv = g_new0 (gchar *, 3);
	argv[0] = text_filter_file;
	argv[1] = (gchar *) path;

	g_message ("Extracting text for:'%s' using filter:'%s'", argv[1], argv[0]);

	context = create_process_context ((const gchar **) argv);

	g_free (text_filter_file);
	g_free (argv);

	if (!context) {
		return NULL;
	}

	text = g_string_new (NULL);
	context->data = text;

	g_io_add_watch (context->stdout_channel,
			G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP,
			tracker_text_read,
			context);

	/* It will block here until all incoming
	 * text has been processed
	 */
	g_main_loop_run (context->data_incoming_loop);

	destroy_process_context (context);

	return g_string_free (text, FALSE);
}

static gboolean 
get_file_is_utf8 (GString *s,
		  gssize  *bytes_valid)
{
	const gchar *end;
	
	/* Check for UTF-8 validity, since we may
	 * have cut off the end.
	 */
	if (g_utf8_validate (s->str, s->len, &end)) {
		*bytes_valid = (gssize) s->len;
		return TRUE;
	}

	*bytes_valid = end - s->str;
	
	/* 4 is the maximum bytes for a UTF-8 character. */
	if (*bytes_valid > 4) {
		return FALSE;
	}

	if (g_utf8_get_char_validated (end, *bytes_valid) == (gunichar) -1) {
		return FALSE;
	}
	
	return TRUE;
}

#ifdef TRY_LOCALE_TO_UTF8_CONVERSION

static GString *
get_file_in_locale (GString *s)
{
	GError *error = NULL;
	gchar  *str;
	gsize   bytes_read;
	gsize   bytes_written;
	
	str = g_locale_to_utf8 (s->str, 
				s->len, 
				&bytes_read, 
				&bytes_written, 
				&error);
	if (error) {
		g_debug ("  Conversion to UTF-8 read %d bytes, wrote %d bytes",
			 bytes_read, 
			 bytes_written);
		g_message ("Could not convert file from locale to UTF-8, %s", 
			   error->message);
		g_error_free (error);
		g_free (str);
	} else {
		g_string_assign (s, str);
		g_free (str);
	}

	return s;
}

#endif /* TRY_LOCALE_TO_UTF8_CONVERSION */

static gchar *
get_file_content (const gchar *path)
{
        GFile            *file;
        GFileInputStream *stream;
        GError           *error = NULL;
	GString          *s;
        gssize            bytes;
        gssize            bytes_valid;
        gssize            bytes_read_total;
	gssize            buf_size;
        gchar             buf[TEXT_CHECK_SIZE];
	gboolean          has_more_data;
	gboolean          has_reached_max;
	gboolean          is_utf8;

        file = g_file_new_for_path (path);
        stream = g_file_read (file, NULL, &error);

        if (error) {
                g_message ("Could not get read file:'%s', %s",
                           path,
                           error->message);
                g_error_free (error);
                g_object_unref (file);

                return NULL;
        }

	s = g_string_new ("");
	has_reached_max = FALSE;
	has_more_data = TRUE;
	bytes_read_total = 0;
	buf_size = TEXT_CHECK_SIZE - 1;

	g_debug ("  Starting read...");
	
	while (has_more_data && !has_reached_max && !error) {
		gssize bytes_read;
		gssize bytes_remaining;

		/* Leave space for NULL termination and make sure we
		 * add it at the end now.
		 */
		bytes_remaining = buf_size;
		bytes_read = 0;

		/* Loop until we hit the maximum */
		for (bytes = -1; bytes != 0 && !error; ) {
			bytes = g_input_stream_read (G_INPUT_STREAM (stream),
						     buf,
						     bytes_remaining,
						     NULL,
						     &error);

			bytes_read += bytes;
			bytes_remaining -= bytes;

			g_debug ("  Read %d bytes", 
				 bytes);
		}

		/* Set the NULL termination after the last byte read */
		buf[TEXT_CHECK_SIZE - bytes_remaining] = '\0';

		/* First of all, check if this is the first time we
		 * have tried to read the file up to the TEXT_CHECK_SIZE
		 * limit. Then make sure that we read the maximum size
		 * of the buffer. If we don't do this, there is the
		 * case where we read 10 bytes in and it is just one
		 * line with no '\n'. Once we have confirmed this we
		 * check that the buffer has a '\n' to make sure the
		 * file is worth indexing. Similarly if the file has
		 * <= 3 bytes then we drop it.
		 */
		if (bytes_read_total == 0) {
			if (bytes_read == buf_size &&
			    strchr (buf, '\n') == NULL) {
				g_debug ("  No '\\n' in the first %d bytes, not indexing file", 
					 buf_size);
				break;
			} else if (bytes_read <= 2) {
				g_debug ("  File has less than 3 characters in it, not indexing file");
				break;
			}
		}

		/* Here we increment the bytes read total to evaluate
		 * the next states. We don't do this before the
		 * previous condition so we can know when we have
		 * iterated > 1.
		 */
		bytes_read_total += bytes_read;

		if (bytes_read != buf_size || bytes_read == 0) {
			has_more_data = FALSE;
		} 

		if (bytes_read_total >= TEXT_MAX_SIZE) {
			has_reached_max = TRUE;
		}

		g_debug ("  Read %d bytes total, %d bytes this time, more data:%s, reached max:%s", 
			 bytes_read_total,
			 bytes_read,
			 has_more_data ? "yes" : "no",
			 has_reached_max ? "yes" : "no");

		s = g_string_append (s, buf);
	}

	if (has_reached_max) {
		g_debug ("  Maximum indexable limit reached");
	}

        if (error) {
                g_message ("Could not read input stream for:'%s', %s",
                           path,
                           error->message);
                g_error_free (error);
                g_object_unref (file);
                g_object_unref (stream);
                g_string_free (s, TRUE);

                return NULL;
        }
	
	/* Check for UTF-8 Validity, if not try to convert it to the
	 * locale we are in.
	 */
	is_utf8 = get_file_is_utf8 (s, &bytes_valid);
	
	/* Make sure the string is NULL terminated and in the case
	 * where the string is valid UTF-8 up to the last character
	 * which was cut off, NULL terminate to the last most valid
	 * character.  
	 */
#ifdef TRY_LOCALE_TO_UTF8_CONVERSION
	if (!is_utf8) {
		s = get_file_in_locale (s);
	} else {
		g_debug ("  Truncating to last valid UTF-8 character (%d/%d bytes)", 
			 bytes_valid,
			 s->len);
		s = g_string_truncate (s, bytes_valid);
	}
#else   /* TRY_LOCALE_TO_UTF8_CONVERSION */
	g_debug ("  Truncating to last valid UTF-8 character (%d/%d bytes)", 
		 bytes_valid,
		 s->len);
	s = g_string_truncate (s, bytes_valid);
#endif  /* TRY_LOCALE_TO_UTF8_CONVERSION */

        g_object_unref (file);
        g_object_unref (stream);

	if (s->len < 1) {
		g_string_free (s, TRUE);
		s = NULL;
	}
	
        return s ? g_string_free (s, FALSE) : NULL;
}

gchar *
tracker_metadata_utils_get_text (const gchar *path)
{
	gchar *mimetype, *service_type;
	gchar *text = NULL;

	mimetype = tracker_file_get_mime_type (path);
	service_type = tracker_ontology_get_service_type_for_mime (mimetype);

	/* No need to filter text based files - index them directly */
	if (service_type &&
            (strcmp (service_type, "Text") == 0 ||
             strcmp (service_type, "Development") == 0)) {
                text = get_file_content (path);
	} else {
		text = call_text_filter (path, mimetype);
	}

	g_free (mimetype);
	g_free (service_type);

	return text;
}

static void
tracker_metadata_utils_get_thumbnail (const gchar *path,
				      const gchar *mime)
{
#ifdef THUMBNAIL_RETRIEVAL_ENABLED
#ifdef HAVE_HILDON_THUMBNAIL
	hildon_thumbnail_factory_load (path, mime, 128, 128, NULL, NULL);
#else
	ProcessContext *context;

	GString *thumbnail;
	gchar *argv[5];

	argv[0] = g_strdup (LIBEXEC_PATH G_DIR_SEPARATOR_S "tracker-thumbnailer");
	argv[1] = g_filename_from_utf8 (path, -1, NULL, NULL, NULL);
	argv[2] = g_strdup (mime);
	argv[3] = g_strdup ("normal");
	argv[4] = NULL;

	context = create_process_context ((const gchar **) argv);

	if (!context) {
		return;
	}

	thumbnail = g_string_new (NULL);
	context->data = thumbnail;

	g_io_add_watch (context->stdout_channel,
			G_IO_IN | G_IO_PRI | G_IO_ERR | G_IO_HUP,
			tracker_text_read,
			context);

	g_main_loop_run (context->data_incoming_loop);

	g_free (argv[0]);
	g_free (argv[1]);
	g_free (argv[2]);
	g_free (argv[3]);

	destroy_process_context (context);

	if (!thumbnail->str || !*thumbnail->str) {
		g_string_free (thumbnail, TRUE);
		return;
	}

	g_debug ("Got thumbnail '%s' for '%s'", thumbnail->str, path);

	g_string_free (thumbnail, TRUE);
#endif /* HAVE_HILDON_THUMBNAIL */
#endif /* THUMBNIAL_RETRIEVAL_ENABLED */
}

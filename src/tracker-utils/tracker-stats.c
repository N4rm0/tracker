/* Tracker - indexer and metadata database engine
 * Copyright (C) 2006, Mr Jamie McCracken (jamiemcc@gnome.org)
 * Copyright (C) 2008, Nokia
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include <config.h>

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <glib.h>
#include <glib/gi18n.h>

#include <src/libtracker/tracker.h>

#define TOTAL_COUNT _("Total files indexed")

static void
get_meta_table_data (gpointer value)
{
	char **meta, **meta_p;

	meta = (char **)value;

	int i = 0;
	for (meta_p = meta; *meta_p; meta_p++) {

		if (i == 0) {
			g_print ("%s : ", *meta_p);

		} else {
			g_print ("%s ", *meta_p);
		}
		i++;
	}
	g_print ("\n");
}



int 
main (int argc, char **argv) 
{
	
	GPtrArray *out_array = NULL;
	GError *error = NULL;
	GOptionContext *context = NULL;
	TrackerClient *client = NULL;

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

        /* Translators: this messagge will apper immediately after the  */
        /* usage string - Usage: COMMAND [OPTION]... <THIS_MESSAGE>     */
        context = g_option_context_new (_(" - Show number of indexed files for each service"));

	g_option_context_parse (context, &argc, &argv, &error);

        g_option_context_free (context);

        if (error) {
                g_printerr ("%s: %s", argv[0], error->message);
                g_printerr ("\n");
                g_printerr (_("Try \"%s --help\" for more information."), argv[0]);
                g_printerr ("\n");
                return 1;
        }

	client =  tracker_connect (FALSE);

        if (!client) {
                g_printerr (_("%s: no connection to tracker daemon"), argv[0]);
                g_printerr ("\n");
                g_printerr (_("Ensure \"trackerd\" is running before launch this command."));
                g_printerr ("\n");
                return 1;
        }

	out_array = tracker_get_stats (client, &error);

	if (error) {
		g_warning ("%s: an error has occurred: %s", argv[0], error->message);
		g_error_free (error);
	}

	if (out_array) {
		gchar *tmp;

		tmp = g_strconcat("\n-------", _("fetching index stats"),
				  "---------\n\n", NULL);

		g_print (tmp);
		g_ptr_array_foreach (out_array, (GFunc)get_meta_table_data, NULL);
		g_ptr_array_free (out_array, TRUE);
		g_print ("------------------------------------\n\n");
		
		g_free (tmp);

	}

	tracker_disconnect (client);

	return EXIT_SUCCESS;
}

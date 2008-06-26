/* Tracker - indexer and metadata database engine
 * Copyright (C) 2007-2008 Nokia
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

/* These defines are required to allow lrintf() without warnings when
 * building.
 */
#define _ISOC9X_SOURCE  1
#define _ISOC99_SOURCE  1

#define __USE_ISOC9X    1
#define __USE_ISOC99    1

#include <string.h>
#include <math.h>
#include <depot.h>

#include <glib-object.h>

#include <libtracker-common/tracker-config.h>
#include <libtracker-common/tracker-parser.h>
#include <libtracker-common/tracker-ontology.h>

#include "tracker-query-tree.h"
#include "tracker-utils.h"

#define TRACKER_QUERY_TREE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), TRACKER_TYPE_QUERY_TREE, TrackerQueryTreePrivate))

#define MAX_HIT_BUFFER 480000
#define SCORE_MULTIPLIER 100000

typedef enum   OperationType OperationType;
typedef enum   TreeNodeType TreeNodeType;
typedef struct TreeNode TreeNode;
typedef struct TrackerQueryTreePrivate TrackerQueryTreePrivate;
typedef struct ComposeHitsData ComposeHitsData;
typedef struct SearchHitData SearchHitData;

enum OperationType {
	OP_NONE,
	OP_AND,
	OP_OR
};

struct TreeNode {
	TreeNode      *left;
	TreeNode      *right;
	OperationType  op;
	gchar         *term;
};

struct TrackerQueryTreePrivate {
	gchar           *query_str;
	TreeNode        *tree;
	TrackerIndexer  *indexer;
        TrackerConfig   *config;
        TrackerLanguage *language;
	GArray          *services;
};

struct ComposeHitsData {
	OperationType  op;
	GHashTable    *other_table;
	GHashTable    *dest_table;
};

struct SearchHitData {
	guint32 service_type_id;
	guint32 score;
};

enum {
	PROP_0,
	PROP_QUERY,
	PROP_INDEXER,
        PROP_CONFIG,
        PROP_LANGUAGE,
	PROP_SERVICES
};

static void tracker_query_tree_class_init   (TrackerQueryTreeClass *class);
static void tracker_query_tree_init         (TrackerQueryTree      *tree);
static void tracker_query_tree_finalize     (GObject               *object);
static void tracker_query_tree_set_property (GObject               *object,
                                             guint                  prop_id,
                                             const GValue          *value,
                                             GParamSpec            *pspec);
static void tracker_query_tree_get_property (GObject               *object,
                                             guint                  prop_id,
                                             GValue                *value,
                                             GParamSpec            *pspec);

G_DEFINE_TYPE (TrackerQueryTree, tracker_query_tree, G_TYPE_OBJECT)

static void
tracker_query_tree_class_init (TrackerQueryTreeClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = tracker_query_tree_finalize;
	object_class->set_property = tracker_query_tree_set_property;
	object_class->get_property = tracker_query_tree_get_property;

	g_object_class_install_property (object_class,
					 PROP_QUERY,
					 g_param_spec_string ("query",
							      "Query",
							      "Query",
							      NULL,
							      G_PARAM_READWRITE));
	g_object_class_install_property (object_class,
					 PROP_INDEXER,
					 g_param_spec_object ("indexer",
                                                              "Indexer",
                                                              "Indexer",
                                                              tracker_indexer_get_type (),
                                                              G_PARAM_READWRITE));
	g_object_class_install_property (object_class,
					 PROP_CONFIG,
					 g_param_spec_object ("config",
                                                              "Config",
                                                              "Config",
                                                              tracker_config_get_type (),
                                                              G_PARAM_READWRITE));
	g_object_class_install_property (object_class,
					 PROP_LANGUAGE,
					 g_param_spec_object ("language",
                                                              "Language",
                                                              "Language",
                                                              tracker_language_get_type (),
                                                              G_PARAM_READWRITE));
	g_object_class_install_property (object_class,
					 PROP_SERVICES,
					 g_param_spec_pointer ("services",
							       "Services",
							       "GArray of services",
							       G_PARAM_READWRITE));
	g_type_class_add_private (object_class,
				  sizeof (TrackerQueryTreePrivate));
}

static void
tracker_query_tree_init (TrackerQueryTree *tree)
{
}

static TreeNode *
tree_node_leaf_new (const gchar *term)
{
	TreeNode *node;

	node = g_slice_new0 (TreeNode);
	node->term = g_strdup (term);
	node->op = OP_NONE;

	return node;
}

static TreeNode *
tree_node_operator_new (OperationType op)
{
	TreeNode *node;

	node = g_slice_new0 (TreeNode);
	node->op = op;

	return node;
}

static void
tree_node_free (TreeNode *node)
{
	if (!node)
		return;

	/* Free string if any */
	g_free (node->term);

	/* free subnodes */
	tree_node_free (node->left);
	tree_node_free (node->right);

	g_slice_free (TreeNode, node);
}

static void
tracker_query_tree_finalize (GObject *object)
{
	TrackerQueryTreePrivate *priv;

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (object);

	tree_node_free (priv->tree);
	g_free (priv->query_str);

	G_OBJECT_CLASS (tracker_query_tree_parent_class)->finalize (object);
}

static void
tracker_query_tree_set_property (GObject      *object,
				 guint         prop_id,
				 const GValue *value,
				 GParamSpec   *pspec)
{
	switch (prop_id) {
	case PROP_QUERY:
		tracker_query_tree_set_query (TRACKER_QUERY_TREE (object),
					      g_value_get_string (value));
		break;
	case PROP_INDEXER:
		tracker_query_tree_set_indexer (TRACKER_QUERY_TREE (object),
						g_value_get_pointer (value));
		break;
	case PROP_CONFIG:
		tracker_query_tree_set_config (TRACKER_QUERY_TREE (object),
                                               g_value_get_object (value));
		break;
	case PROP_LANGUAGE:
		tracker_query_tree_set_language (TRACKER_QUERY_TREE (object),
						 g_value_get_object (value));
		break;
	case PROP_SERVICES:
		tracker_query_tree_set_services (TRACKER_QUERY_TREE (object),
						 g_value_get_pointer (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	}
}

static void
tracker_query_tree_get_property (GObject      *object,
				 guint         prop_id,
				 GValue       *value,
				 GParamSpec   *pspec)
{
	TrackerQueryTreePrivate *priv;

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_QUERY:
		g_value_set_string (value, priv->query_str);
		break;
	case PROP_INDEXER:
		g_value_set_object (value, priv->indexer);
		break;
	case PROP_CONFIG:
		g_value_set_object (value, priv->config);
		break;
	case PROP_LANGUAGE:
		g_value_set_object (value, priv->language);
		break;
	case PROP_SERVICES:
		g_value_set_pointer (value, priv->services);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	}
}

TrackerQueryTree *
tracker_query_tree_new (const gchar     *query_str,
			TrackerIndexer  *indexer,
                        TrackerConfig   *config,
                        TrackerLanguage *language,
			GArray          *services)
{
	g_return_val_if_fail (query_str != NULL, NULL);
	g_return_val_if_fail (TRACKER_IS_INDEXER (indexer), NULL);
	g_return_val_if_fail (TRACKER_IS_CONFIG (config), NULL);
	g_return_val_if_fail (language != NULL, NULL);

        /* NOTE: The "query" has to come AFTER the "config" and
         * "language" properties since setting the query actually
         * uses the priv->config and priv->language settings.
         * Changing this order results in warnings.
         */
	return g_object_new (TRACKER_TYPE_QUERY_TREE,
			     "indexer", indexer,
                             "config", config,
                             "language", language,
			     "services", services,
			     "query", query_str,
			     NULL);
}

#if 0
static void
print_tree (TreeNode *node)
{
	if (!node) {
		g_print ("NULL ");
		return;
	}

	switch (node->op) {
	case OP_AND:
		g_print ("AND ");
		print_tree (node->left);
		print_tree (node->right);
		break;
	case OP_OR:
		g_print ("OR ");
		print_tree (node->left);
		print_tree (node->right);
		break;
	default:
		g_print ("%s ", node->term);
	}
}
#endif

static void
create_query_tree (TrackerQueryTree *tree)
{
	TrackerQueryTreePrivate *priv;
	TreeNode *node, *stack_node;
	GQueue *queue, *stack;
	gboolean last_element_is_term = FALSE;
	gchar *parsed_str, **strings;
	gint i;

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (tree);

	strings = g_strsplit (priv->query_str, " ", -1);
	queue = g_queue_new ();
	stack = g_queue_new ();

	/* Create a parse tree that recognizes queries such as:
	 * "foo"
	 * "foo and bar"
	 * "foo or bar"
	 * "foo and bar or baz"
	 * "foo or bar and baz"
	 *
	 * The operator "and" will have higher priority than the operator "or",
	 * and will also be assumed to exist if there is no operator between two
	 * search terms.
	 */

	/* Step 1. Create polish notation for the search term, the
	 * stack will be used to store operators temporarily
	 */
	for (i = 0; strings[i]; i++) {
		OperationType op;

		if (!strings[i] || !*strings[i])
			continue;

		/* get operator type */
		if (strcmp (strings[i], "and") == 0) {
			op = OP_AND;
		} else if (strcmp (strings [i], "or") == 0) {
			op = OP_OR;
		} else {
			if (last_element_is_term) {
				/* last element was a search term, assume the "and"
				 * operator between these two elements, and wait
				 * for actually parsing the second term */
				op = OP_AND;
				i--;
			} else {
				op = OP_NONE;
			}
		}

		/* create node for the operation type */
		switch (op) {
		case OP_AND:
			node = tree_node_operator_new (OP_AND);
			stack_node = g_queue_peek_head (stack);

			/* push in the queue operators with fewer priority, "or" in this case */
			while (stack_node && stack_node->op == OP_OR) {
				stack_node = g_queue_pop_head (stack);
				g_queue_push_head (queue, stack_node);

				stack_node = g_queue_peek_head (stack);
			}

			g_queue_push_head (stack, node);
			last_element_is_term = FALSE;
			break;
		case OP_OR:
			node = tree_node_operator_new (OP_OR);
			g_queue_push_head (stack, node);
			last_element_is_term = FALSE;
			break;
		default:
			/* search term */
			parsed_str = tracker_parser_text_to_string (strings[i], 
                                                                    priv->language,
                                                                    tracker_config_get_max_word_length (priv->config),
                                                                    tracker_config_get_min_word_length (priv->config),
                                                                    TRUE, 
                                                                    FALSE, 
                                                                    FALSE);
			node = tree_node_leaf_new (g_strstrip (parsed_str));
			g_queue_push_head (queue, node);
			last_element_is_term = TRUE;

			g_free (parsed_str);
		}
	}

	/* we've finished parsing, queue all remaining operators in the stack */
	while ((stack_node = g_queue_pop_head (stack)) != NULL) {
		g_queue_push_head (queue, stack_node);
	}

	/* step 2: run through the reverse polish notation and connect nodes to
	 * create a tree, the stack will be used to store temporarily nodes
	 * until they're connected to a parent node */
	while ((node = g_queue_pop_tail (queue)) != NULL) {
		switch (node->op) {
		case OP_AND:
		case OP_OR:
			node->left = g_queue_pop_head (stack);
			node->right = g_queue_pop_head (stack);
			g_queue_push_head (stack, node);
			break;
		default:
			g_queue_push_head (stack, node);
			break;
		}

		priv->tree = node;
	}

	g_strfreev (strings);
	g_queue_free (stack);
	g_queue_free (queue);
}

void
tracker_query_tree_set_query (TrackerQueryTree *tree,
			      const gchar      *query_str)
{
	TrackerQueryTreePrivate *priv;
	gchar *str;

	g_return_if_fail (TRACKER_IS_QUERY_TREE (tree));
	g_return_if_fail (query_str != NULL);

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (tree);

	str = g_strdup (query_str);
	g_free (priv->query_str);
	priv->query_str = str;

	/* construct the parse tree */
	create_query_tree (tree);

	g_object_notify (G_OBJECT (tree), "query");
}

G_CONST_RETURN gchar *
tracker_query_tree_get_query (TrackerQueryTree *tree)
{
	TrackerQueryTreePrivate *priv;

	g_return_val_if_fail (TRACKER_IS_QUERY_TREE (tree), NULL);

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (tree);
	return priv->query_str;
}

void
tracker_query_tree_set_indexer (TrackerQueryTree *tree,
				TrackerIndexer   *indexer)
{
	TrackerQueryTreePrivate *priv;

	g_return_if_fail (TRACKER_IS_QUERY_TREE (tree));
	g_return_if_fail (TRACKER_IS_INDEXER (indexer));

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (tree);

	if (indexer) {
		g_object_ref (indexer);
	}

	if (priv->indexer) {
		g_object_unref (priv->indexer);
	}

	priv->indexer = indexer;

	g_object_notify (G_OBJECT (tree), "indexer");
}

TrackerIndexer *
tracker_query_tree_get_indexer (TrackerQueryTree *tree)
{
	TrackerQueryTreePrivate *priv;

	g_return_val_if_fail (TRACKER_IS_QUERY_TREE (tree), NULL);

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (tree);
	return priv->indexer;
}

void
tracker_query_tree_set_config (TrackerQueryTree *tree,
			       TrackerConfig    *config)
{
	TrackerQueryTreePrivate *priv;

	g_return_if_fail (TRACKER_IS_QUERY_TREE (tree));
	g_return_if_fail (TRACKER_IS_CONFIG (config));

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (tree);

	if (config) {
		g_object_ref (config);
	}

	if (priv->config) {
		g_object_unref (priv->config);
	}

	priv->config = config;

	g_object_notify (G_OBJECT (tree), "config");
}

TrackerConfig *
tracker_query_tree_get_config (TrackerQueryTree *tree)
{
	TrackerQueryTreePrivate *priv;

	g_return_val_if_fail (TRACKER_IS_QUERY_TREE (tree), NULL);

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (tree);
	return priv->config;
}

void
tracker_query_tree_set_language (TrackerQueryTree *tree,
                                 TrackerLanguage  *language)
{
	TrackerQueryTreePrivate *priv;

	g_return_if_fail (TRACKER_IS_QUERY_TREE (tree));
	g_return_if_fail (language != NULL);

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (tree);

	if (language) {
		g_object_ref (language);
	}

	if (priv->language) {
		g_object_unref (priv->language);
	}

	priv->language = language;

	g_object_notify (G_OBJECT (tree), "language");
}

TrackerLanguage *
tracker_query_tree_get_language (TrackerQueryTree *tree)
{
	TrackerQueryTreePrivate *priv;

	g_return_val_if_fail (TRACKER_IS_QUERY_TREE (tree), NULL);

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (tree);
	return priv->language;
}

void
tracker_query_tree_set_services (TrackerQueryTree *tree,
				 GArray           *services)
{
	TrackerQueryTreePrivate *priv;
	GArray *copy = NULL;

	g_return_if_fail (TRACKER_IS_QUERY_TREE (tree));

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (tree);

	if (priv->services != services) {
		if (services) {
			copy = g_array_new (TRUE, TRUE, sizeof (gint));
			g_array_append_vals (copy, services->data, services->len);
		}

		if (priv->services)
			g_array_free (priv->services, TRUE);

		priv->services = copy;
		g_object_notify (G_OBJECT (tree), "services");
	}
}

GArray *
tracker_query_tree_get_services (TrackerQueryTree *tree)
{
	TrackerQueryTreePrivate *priv;

	g_return_val_if_fail (TRACKER_IS_QUERY_TREE (tree), NULL);

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (tree);
	return priv->services;
}

static void
get_tree_words (TreeNode *node, GSList **list)
{
	if (!node)
		return;

	if (node->op == OP_NONE)
		*list = g_slist_prepend (*list, node->term);
	else {
		get_tree_words (node->left, list);
		get_tree_words (node->right, list);
	}
}

GSList *
tracker_query_tree_get_words (TrackerQueryTree *tree)
{
	TrackerQueryTreePrivate *priv;
	GSList *list = NULL;

	g_return_val_if_fail (TRACKER_IS_QUERY_TREE (tree), NULL);

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (tree);
	get_tree_words (priv->tree, &list);

	return list;
}

static gint
get_idf_score (TrackerIndexerWordDetails *details, 
               gfloat                     idf)
{
	guint32 score;
	gfloat  f;

        score = tracker_indexer_word_details_get_score (details);
        f = idf * score * SCORE_MULTIPLIER;

        return (f > 1.0) ? lrintf (f) : 1;
}

static gboolean
in_array (GArray *array, gint element)
{
	guint i;

	if (!array)
		return TRUE;

	for (i = 0; i < array->len; i++) {
		if (g_array_index (array, gint, i) == element)
			return TRUE;
	}

	return FALSE;
}

static void
search_hit_data_free (SearchHitData *search_hit)
{
	g_slice_free (SearchHitData, search_hit);
}

static GHashTable *
get_search_term_hits (TrackerQueryTree *tree,
		      const gchar      *term)
{
	TrackerQueryTreePrivate *priv;
	TrackerIndexerWordDetails *details;
	GHashTable *result;
	guint count, i;

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (tree);
	result = g_hash_table_new_full (NULL, NULL, NULL,
					(GDestroyNotify) search_hit_data_free);

	details = tracker_indexer_get_word_hits (priv->indexer, term, &count);

	if (!details)
		return result;

	for (i = 0; i < count; i++) {
		SearchHitData *data;
		gint service;

		service = tracker_indexer_word_details_get_service_type (&details[i]);

		if (in_array (priv->services, service)) {
			data = g_slice_new (SearchHitData);
			data->service_type_id = service;
			data->score = get_idf_score (&details[i], (float) 1 / count);

			g_hash_table_insert (result, GINT_TO_POINTER (details[i].id), data);
		}
	}

	g_free (details);

	return result;
}

static void
compose_hits_foreach (gpointer key,
		      gpointer value,
		      gpointer user_data)
{
	SearchHitData *hit1, *hit2, *hit;
	ComposeHitsData *data;

	data = (ComposeHitsData *) user_data;
	hit1 = (SearchHitData *) value;
	hit2 = g_hash_table_lookup (data->other_table, key);

	if (data->op == OP_OR) {
		/* compose both scores in the same entry */
		hit = g_slice_dup (SearchHitData, hit1);

		if (hit2)
			hit->score += hit2->score;

		g_hash_table_insert (data->dest_table, key, hit);
	} else if (data->op == OP_AND) {
		/* only compose if the key is in both tables */
		if (hit2) {
			hit = g_slice_dup (SearchHitData, hit1);
			hit->score += hit2->score;
			g_hash_table_insert (data->dest_table, key, hit);
		}
	} else {
		g_assert_not_reached ();
	}
}

static GHashTable *
compose_hits (OperationType  op,
	      GHashTable    *left_table,
	      GHashTable    *right_table)
{
	ComposeHitsData data;
	GHashTable *foreach_table;

	data.op = op;

	/* try to run the foreach in the table with less hits */
	if (g_hash_table_size (left_table) < g_hash_table_size (right_table)) {
		foreach_table = left_table;
		data.other_table = right_table;
	} else {
		foreach_table = right_table;
		data.other_table = left_table;
	}

	if (op == OP_OR) {
		data.dest_table = g_hash_table_ref (data.other_table);
	} else {
		data.dest_table = g_hash_table_new_full (NULL, NULL, NULL,
							 (GDestroyNotify) search_hit_data_free);
	}

	g_hash_table_foreach (foreach_table, (GHFunc) compose_hits_foreach, &data);

	return data.dest_table;
}

static GHashTable *
get_node_hits (TrackerQueryTree *tree,
	       TreeNode         *node)
{
	GHashTable *left_table, *right_table, *result;

	if (!node)
		return NULL;

	switch (node->op) {
	case OP_NONE:
		result = get_search_term_hits (tree, node->term);
		break;
	case OP_AND:
	case OP_OR:
		left_table = get_node_hits (tree, node->left);
		right_table = get_node_hits (tree, node->right);
		result = compose_hits (node->op, left_table, right_table);

		g_hash_table_unref (left_table);
		g_hash_table_unref (right_table);
		break;
	default:
		g_assert_not_reached ();
	}

	return result;
}

static void
get_hits_foreach (gpointer key,
		  gpointer value,
		  gpointer user_data)
{
	GArray *array;
	TrackerSearchHit hit;
	SearchHitData *hit_data;

	array = (GArray *) user_data;
	hit_data = (SearchHitData *) value;

	hit.service_id = GPOINTER_TO_UINT (key);
	hit.service_type_id = hit_data->service_type_id;
	hit.score = hit_data->score;

	g_array_append_val (array, hit);
}

static gint
compare_search_hits (gconstpointer a,
		     gconstpointer b)
{
	TrackerSearchHit *ap, *bp;

	ap = (TrackerSearchHit *) a;
	bp = (TrackerSearchHit *) b;

	return (bp->score - ap->score);
}

GArray *
tracker_query_tree_get_hits (TrackerQueryTree *tree,
			     guint             offset,
			     guint             limit)
{
	TrackerQueryTreePrivate *priv;
	GHashTable *table;
	GArray *array;

	g_return_val_if_fail (TRACKER_IS_QUERY_TREE (tree), NULL);

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (tree);

	g_return_val_if_fail (priv->tree != NULL, NULL);

	table = get_node_hits (tree, priv->tree);
	array = g_array_sized_new (TRUE, TRUE, sizeof (TrackerSearchHit),
				   g_hash_table_size (table));

	g_hash_table_foreach (table, (GHFunc) get_hits_foreach, array);
	g_array_sort (array, compare_search_hits);

	if (offset > 0) {
		g_array_remove_range (array, 0, CLAMP (offset, 0, array->len));
	}

	if (limit > 0 && limit < array->len) {
		g_array_remove_range (array, limit, array->len - limit);
	}

	return array;
}

gint
tracker_query_tree_get_hit_count (TrackerQueryTree *tree)
{
	TrackerQueryTreePrivate *priv;
	GHashTable *table;
	gint count;

	g_return_val_if_fail (TRACKER_IS_QUERY_TREE (tree), 0);

	priv = TRACKER_QUERY_TREE_GET_PRIVATE (tree);
	table = get_node_hits (tree, priv->tree);

	if (!table)
		return 0;

	count = g_hash_table_size (table);
	g_hash_table_destroy (table);

	return count;
}

static void
get_hit_count_foreach (gpointer key,
		       gpointer value,
		       gpointer user_data)
{
	GArray *array = (GArray *) user_data;
	TrackerHitCount count;

	count.service_type_id = GPOINTER_TO_INT (key);
	count.count = GPOINTER_TO_INT (value);

	g_array_append_val (array, count);
}

GArray *
tracker_query_tree_get_hit_counts (TrackerQueryTree *tree)
{
	GHashTable *table;
	GArray *hits, *counts;
	guint i;

	g_return_val_if_fail (TRACKER_IS_QUERY_TREE (tree), NULL);

	table = g_hash_table_new (NULL, NULL);
	hits = tracker_query_tree_get_hits (tree, 0, 0);
	counts = g_array_sized_new (TRUE, TRUE, sizeof (TrackerHitCount), 10);

	for (i = 0; i < hits->len; i++) {
		TrackerSearchHit hit;
		gint count, parent_id;

		hit = g_array_index (hits, TrackerSearchHit, i);
		count = GPOINTER_TO_INT (g_hash_table_lookup (table, GINT_TO_POINTER (hit.service_type_id)));
		count++;

		g_hash_table_insert (table, GINT_TO_POINTER (hit.service_type_id), GINT_TO_POINTER (count));

		/* update service's parent count too (if it has a parent) */
		parent_id = tracker_ontology_get_parent_id_for_service_id (hit.service_type_id);

		if (parent_id != -1) {
			count = GPOINTER_TO_INT (g_hash_table_lookup (table, GINT_TO_POINTER (parent_id)));
			count++;

			g_hash_table_insert (table, GINT_TO_POINTER (parent_id), GINT_TO_POINTER (count));
		}
	}

	g_hash_table_foreach (table, (GHFunc) get_hit_count_foreach, counts);
	g_array_free (hits, TRUE);

	return counts;
}

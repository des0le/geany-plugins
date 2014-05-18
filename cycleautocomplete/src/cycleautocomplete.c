/*
 * cycleautocomplete.c -- part of Geany-Plugins
 *
 * Copyright (c) 2014  Yannick Lipp <desole@kabsi.at>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <geanyplugin.h>

GeanyPlugin *geany_plugin;
GeanyData *geany_data;
GeanyFunctions *geany_functions;

PLUGIN_VERSION_CHECK(216)

/* GETTEXT_PACKAGE and LOCALEDIR are only set when using the Geany-Plugin build system
 * by conifg.h and configure, respectively */
PLUGIN_SET_TRANSLATABLE_INFO(
	LOCALEDIR,
	GETTEXT_PACKAGE,
	_("Cycle-Autocomplete"),
	_("Inline autocompletion based on words in current document."),
	"1.0",
	"Yannick Lipp <desole@kabsi.at>"
	)


typedef gboolean (*SelectFunc)(const gchar *pattern, const gchar* match);


typedef enum
{
	SORT_ALPHABETICALLY,
	SORT_BY_DISTANCE,
	SORT_COUNT
} SortOrder;


typedef enum
{
	MATCH_EXACT,
	MATCH_FUZZY_FORWARD,
	MATCH_COUNT
} MatchType;


typedef enum
{
	CYCLE_FORWARD,
	CYCLE_BACKWARD,
	CYCLE_COUNT
} CycleDirection;


typedef enum
{
	KB_CYCLE_FORWARD,
	KB_CYCLE_BACKWARD,
	KB_COUNT
} KeyBinding;


typedef struct
{
	gint dist;
	gchar *text;
	MatchType match_type;
} Candidate;


typedef struct
{
	gchar *config_file_path;
	gint candidates_limit;
	gint distance_limit;
	gint sort_order;
	gboolean skip_fuzzy_if_exact;
	gboolean remove_trailing_word_part;
} PluginConfig;


static PluginConfig *plugin_config;
static GList *candidates = NULL;


/* these Scintilla wrapper functions are not yet in the plugin API -- see sciwrappers.c */
#ifndef sci_word_start_position
static gint sci_word_start_position(ScintillaObject *sci, gint pos, gboolean onlyWordCharacters)
{
	return (gint)scintilla_send_message(sci, SCI_WORDSTARTPOSITION, (uptr_t)pos, onlyWordCharacters);
}
#endif


#ifndef sci_word_end_position
static gint sci_word_end_position(ScintillaObject *sci, gint pos, gboolean onlyWordCharacters)
{
	return (gint)scintilla_send_message(sci, SCI_WORDENDPOSITION, (uptr_t)pos, onlyWordCharacters);
}
#endif


static void cancel_autocomplete_popup(ScintillaObject *sci)
{
	sci_send_command(sci, SCI_AUTOCCANCEL);
}


static void free_plugin_config(PluginConfig *pc)
{
	g_free(pc->config_file_path);
	g_free(pc);
}


static void free_candidate(Candidate *c)
{
	g_free(c->text);
	g_free(c);
}


static gint compare_candidate_str(const Candidate *c, const gchar *s)
{
	return g_strcmp0(c->text, s);
}


static gint compare_candidates_alpha(const Candidate *c0, const Candidate *c1)
{
	return g_strcmp0(c0->text, c1->text);
}


static gint compare_candidates_dist(const Candidate *c0, const Candidate *c1)
{
	return (c0->dist > c1->dist) - (c0->dist < c1->dist);
}


static gint compare_candidates_match_type(const Candidate *c0, const Candidate *c1)
{
	return (c0->match_type > c1->match_type) - (c0->match_type < c1->match_type);
}


static gboolean match_fuzzy_forward(const gchar *pattern, const gchar* match)
{
	gchar *match_, *pattern_, *baseline, *remaining;
	gboolean answer = TRUE;

	baseline = match_ = g_utf8_casefold(match, -1);
	remaining = pattern_ = g_utf8_casefold(pattern, -1);

	while (remaining && *remaining)
	{
		baseline = g_utf8_strchr(baseline, -1, g_utf8_get_char(remaining));
		if (!baseline)
		{
			answer = FALSE;
			break;
		}
		baseline = g_utf8_next_char(baseline);
		remaining = g_utf8_next_char(remaining);
	}

	g_free(match_);
	g_free(pattern_);
	return answer;
}


static gint find_words(ScintillaObject *sci, const gchar *prefix, const gchar *word, gint pos,
	SelectFunc select_func)
{
	gint radius, source_start, source_end;
	gchar *pattern;
	struct Sci_TextToFind ttf;
	gint flags;
	gint num_matches = 0;
	gchar *match;
	gint match_start, match_end;
	GList *elem = NULL;
	MatchType match_type = MATCH_EXACT;
	Candidate *c;

	if (plugin_config->distance_limit > 0)
	{
		radius = (plugin_config->distance_limit)*1024;
		source_start = MAX(pos - radius, 0);
		source_end = MIN(pos + radius, sci_get_length(sci));
	}
	/* no limit */
	else
	{
		source_start = 0;
		source_end = sci_get_length(sci);
	}

	if (select_func)
	{
		pattern = g_utf8_substring(prefix, 0, 1);
		match_type = MATCH_FUZZY_FORWARD;
	}
	else
	{
		pattern = g_strdup(prefix);
	}

	/* search pattern */
	ttf.lpstrText = pattern;
	/* search range */
	ttf.chrg.cpMin = source_start;
	ttf.chrg.cpMax = source_end;
	/* start and end position of matching text */
	ttf.chrgText.cpMin = 0;
	ttf.chrgText.cpMax = 0;
	/* match only if the character before is not a word character */
	flags = SCFIND_WORDSTART;

	/* the return value of SCI_FINDTEXT is -1 if nothing is found, otherwise
	 * the return value is the start position of the matching text */
	match_start = sci_find_text(sci, flags, &ttf);
	while (match_start >= source_start && match_start < source_end)
	{
		match_end = sci_word_end_position(sci, match_start + 1, TRUE);
		match = sci_get_contents_range(sci, match_start, match_end);
		if (!select_func || select_func(prefix, match))
		{
			elem = g_list_find_custom(candidates, match, (GCompareFunc)&compare_candidate_str);
			if (elem)
			{
				c = (Candidate *)elem->data;
				c->dist = MIN(ABS(match_start - pos), c->dist);
			}
			/* exclude instances of the word itself; it will later be appended to the final list */
			else if (!utils_str_equal(word, match))
			{
				c = g_new(Candidate, 1);
				c->text = g_strdup(match);
				c->dist = ABS(match_start - pos);
				c->match_type = match_type;
				candidates = g_list_prepend(candidates, c);
				++num_matches;
			}
		}
		g_free(match);
		if (num_matches == plugin_config->candidates_limit)
		{
			break;
		}
		ttf.chrg.cpMin = match_end;
		match_start = sci_find_text(sci, flags, &ttf);
	}
	g_free(pattern);
	return num_matches;
}


static void find_candidates(ScintillaObject *sci, const gchar *prefix, const gchar *word, gint pos)
{
	Candidate *c;
	GCompareFunc candidate_sort_func;

	/* exact prefix matching */
	if (!find_words(sci, prefix, word, pos, NULL) || !plugin_config->skip_fuzzy_if_exact)
	{
		/* fuzzy prefix matching */
		find_words(sci, prefix, word, pos, &match_fuzzy_forward);
	}

	/* sort candidates */
	if (candidates)
	{
		switch(plugin_config->sort_order)
		{
		case SORT_ALPHABETICALLY:

			candidate_sort_func = (GCompareFunc)&compare_candidates_alpha;
			break;

		case SORT_BY_DISTANCE:
		default:

			candidate_sort_func = (GCompareFunc)&compare_candidates_dist;
			break;
		}
		candidates = g_list_sort(candidates, candidate_sort_func);
		/* place exact before fuzzy matches (g_list_sort is stable) */
		if (!plugin_config->skip_fuzzy_if_exact)
		{
			candidates = g_list_sort(candidates, (GCompareFunc)&compare_candidates_match_type);
		}
		/* append the current prefix/word to end of list */
		c = g_new0(Candidate, 1);
		c->text = plugin_config->remove_trailing_word_part ? g_strdup(word) : g_strdup(prefix);
		candidates = g_list_append(candidates, c);
	}
}


static gchar *cycle_candidates(CycleDirection direction, const gchar *prev_completion)
{
	Candidate *c;
	GList *elem = NULL;

	if (prev_completion)
	{
		for (elem = candidates; elem; elem = elem->next)
		{
			c = (Candidate *)elem->data;
			if (utils_str_equal(c->text, prev_completion))
			{
				if (direction == CYCLE_FORWARD)
				{
					if (elem->next)
					{
						c = (Candidate *)elem->next->data;
					}
					else
					{
						c = (Candidate *)candidates->data;
					}
				}
				else if (direction == CYCLE_BACKWARD)
				{
					if (elem->prev)
					{
						c = (Candidate *)elem->prev->data;
					}
					else
					{
						elem = g_list_last(elem);
						c = (Candidate *)elem->data;
					}
				}
				return c->text;
			}
		}
	}
	/* first time pick first */
	c = (Candidate *)candidates->data;
	return c->text;
}


static void insert_completion(CycleDirection direction)
{
	static gchar *prev_completion = NULL;
	GeanyDocument *doc;
	ScintillaObject *sci;
	gint pos, start, end;
	gchar *prefix, *word;
	gchar *completion;

	doc = document_get_current();

	g_return_if_fail(doc != NULL);

	sci = doc->editor->sci;

	/* sci_get_current_position depends on the direction of the selection,
	 * whereas sci_get_selection_start is always the left end of it */
	pos = sci_get_selection_start(sci);
	start = sci_word_start_position(sci, pos, TRUE);
	end = sci_word_end_position(sci, pos, TRUE);

	/* this triggers on cases where the cursor is in front of a word (no prefix) or
	 * when the cursor is not touching any of the word characters set by GEANY_WORDCHARS */
	g_return_if_fail(pos > start);

	prefix = sci_get_contents_range(sci, start, pos);
	word = sci_get_contents_range(sci, start, end);

	/* find candidates if there is a new prefix or no previous completion */
	if (!prev_completion || !utils_str_equal(prefix, prev_completion))
	{
		g_free(prev_completion);
		prev_completion = NULL;

		g_list_free_full(candidates, (GDestroyNotify)&free_candidate);
		/* must be NULL to be considered an empty list when using g_list_prepend */
		candidates = NULL;

		find_candidates(sci, prefix, word, pos);
	}

	if (candidates)
	{
		completion = cycle_candidates(direction, prev_completion);

		/* make Geany's default autocompletion popup disappear as well */
		cancel_autocomplete_popup(sci);

		sci_start_undo_action(sci);

		/* replace word with completion */
		sci_set_target_start(sci, start);
		sci_set_target_end(sci, plugin_config->remove_trailing_word_part ? end : pos);
		sci_replace_target(sci, completion, FALSE);
		sci_set_current_position(sci, start + strlen(completion), FALSE);

		sci_end_undo_action(sci);

		prev_completion = g_strdup(completion);
	}
	else
	{
		ui_set_statusbar(FALSE, _("No completions found for \"%s\"."), prefix);
	}

	g_free(prefix);
	g_free(word);
}


static void kb_cycle_forward(G_GNUC_UNUSED guint key_id)
{
	insert_completion(CYCLE_FORWARD);
}


static void kb_cycle_backward(G_GNUC_UNUSED guint key_id)
{
	insert_completion(CYCLE_BACKWARD);
}


static void configure_response_cb(GtkDialog *dialog, gint response, gpointer user_data)
{
	GKeyFile *config_file;
	gchar *config_dir, *data;

	if (response == GTK_RESPONSE_OK || response == GTK_RESPONSE_APPLY)
	{
		/* update config */
		plugin_config->sort_order = (gtk_combo_box_get_active(GTK_COMBO_BOX(
			g_object_get_data(G_OBJECT(dialog), "combo_sort_order"))));
		plugin_config->candidates_limit = (gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(
			g_object_get_data(G_OBJECT(dialog), "spin_candidates_limit"))));
		plugin_config->distance_limit = 1024*(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(
			g_object_get_data(G_OBJECT(dialog), "spin_distance_limit"))));
		plugin_config->skip_fuzzy_if_exact = (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(
			g_object_get_data(G_OBJECT(dialog), "check_skip_fuzzy_if_exact"))));
		plugin_config->remove_trailing_word_part = (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(
			g_object_get_data(G_OBJECT(dialog), "check_remove_trailing_word_part"))));

		/* save config to file */
		config_file = g_key_file_new();
		g_key_file_load_from_file(config_file, plugin_config->config_file_path, G_KEY_FILE_NONE, NULL);

		g_key_file_set_integer(config_file, "cycle_autocomplete", "sort_order",
			plugin_config->sort_order);
		g_key_file_set_integer(config_file, "cycle_autocomplete", "candidates_limit",
			plugin_config->candidates_limit);
		g_key_file_set_integer(config_file, "cycle_autocomplete", "distance_limit",
			plugin_config->distance_limit);
		g_key_file_set_boolean(config_file, "cycle_autocomplete", "skip_fuzzy_if_exact",
			plugin_config->skip_fuzzy_if_exact);
		g_key_file_set_boolean(config_file, "cycle_autocomplete", "remove_trailing_word_part",
			plugin_config->remove_trailing_word_part);

		config_dir = g_path_get_dirname(plugin_config->config_file_path);
		if (!g_file_test(config_dir, G_FILE_TEST_IS_DIR) && utils_mkdir(config_dir, TRUE) != 0)
		{
			dialogs_show_msgbox(GTK_MESSAGE_ERROR,
				_("Plugin configuration directory could not be created."));
		}
		else
		{
			data = g_key_file_to_data(config_file, NULL, NULL);
			utils_write_file(plugin_config->config_file_path, data);
			g_free(data);
		}
		g_free(config_dir);
		g_key_file_free(config_file);
	}
}


void plugin_init(GeanyData *data)
{
	GKeyFile *config_file;
	GeanyKeyGroup *key_group;

	plugin_config = g_new(PluginConfig, 1);
	plugin_config->config_file_path = g_strconcat(geany->app->configdir, G_DIR_SEPARATOR_S, "plugins",
		G_DIR_SEPARATOR_S, "cycleautocomplete", G_DIR_SEPARATOR_S, "cycleautocomplete.conf", NULL);

	/* load config from file */
	config_file = g_key_file_new();
	g_key_file_load_from_file(config_file, plugin_config->config_file_path, G_KEY_FILE_NONE, NULL);

	plugin_config->sort_order = utils_get_setting_integer(config_file,
		"cycle_autocomplete", "sort_order", SORT_BY_DISTANCE);
	plugin_config->candidates_limit = utils_get_setting_integer(config_file,
		"cycle_autocomplete", "candidates_limit", 12);
	plugin_config->distance_limit = utils_get_setting_integer(config_file,
		"cycle_autocomplete", "distance_limit", 0);
	plugin_config->skip_fuzzy_if_exact = utils_get_setting_boolean(config_file,
		"cycle_autocomplete", "skip_fuzzy_if_exact", FALSE);
	plugin_config->remove_trailing_word_part = utils_get_setting_boolean(config_file,
		"cycle_autocomplete", "remove_trailing_word_part", FALSE);

	g_key_file_free(config_file);

	/* setup keybindings */
	key_group = plugin_set_key_group(geany_plugin, "cycle_autocomplete", KB_COUNT, NULL);

	keybindings_set_item(key_group, KB_CYCLE_FORWARD, &kb_cycle_forward, 0, 0,
		"cycle_autocomplete_forward", _("Cycle autocomplete forward"), NULL);

	keybindings_set_item(key_group, KB_CYCLE_BACKWARD, &kb_cycle_backward, 0, 0,
		"cycle_autocomplete_backward", _("Cycle autocomplete backward"), NULL);
}


GtkWidget *plugin_configure(GtkDialog *dialog)
{
	GtkWidget *vbox, *hbox;
	GtkWidget *label;
	GtkWidget *combo_sort_order;
	GtkWidget *spin_candidates_limit;
	GtkWidget *spin_distance_limit;
	GtkWidget *check_skip_fuzzy_if_exact;
	GtkWidget *check_remove_trailing_word_part;

	vbox = gtk_vbox_new(FALSE, 4);

	hbox = gtk_hbox_new(FALSE, 4);
	label = gtk_label_new(_("Sort completions"));
	gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
	combo_sort_order = gtk_combo_box_text_new();
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_sort_order), _("alphabetically"));
	gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_sort_order), _("by distance"));
	gtk_combo_box_set_active(GTK_COMBO_BOX(combo_sort_order), plugin_config->sort_order);
	gtk_box_pack_start(GTK_BOX(hbox), combo_sort_order, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

	hbox = gtk_hbox_new(FALSE, 4);
	label = gtk_label_new(_("Limit number of possible completions"));
	gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
	spin_candidates_limit = gtk_spin_button_new_with_range(1.0, 100.0, 1.0);
	gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin_candidates_limit), 0);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_candidates_limit),
		(gdouble)plugin_config->candidates_limit);
	gtk_box_pack_start(GTK_BOX(hbox), spin_candidates_limit, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

	hbox = gtk_hbox_new(FALSE, 4);
	label = gtk_label_new(_("Limit completion search radius [kbyte]"));
	gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
	spin_distance_limit = gtk_spin_button_new_with_range(0.0, 100.0, 1.0);
	gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin_distance_limit), 0);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_distance_limit),
		(gdouble)plugin_config->distance_limit/1024);
	gtk_box_pack_start(GTK_BOX(hbox), spin_distance_limit, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

	check_skip_fuzzy_if_exact = gtk_check_button_new_with_label(
		_("Skip fuzzy matching if there are exact matches"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_skip_fuzzy_if_exact),
		(gboolean)plugin_config->skip_fuzzy_if_exact);
	gtk_box_pack_start(GTK_BOX(vbox), check_skip_fuzzy_if_exact, FALSE, FALSE, 0);

	check_remove_trailing_word_part = gtk_check_button_new_with_label(
		_("Remove trailing word part on completion"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_remove_trailing_word_part),
		(gboolean)plugin_config->remove_trailing_word_part);
	gtk_box_pack_start(GTK_BOX(vbox), check_remove_trailing_word_part, FALSE, FALSE, 0);

	g_object_set_data(G_OBJECT(dialog), "combo_sort_order", combo_sort_order);
	g_object_set_data(G_OBJECT(dialog), "spin_candidates_limit", spin_candidates_limit);
	g_object_set_data(G_OBJECT(dialog), "spin_distance_limit", spin_distance_limit);
	g_object_set_data(G_OBJECT(dialog), "check_skip_fuzzy_if_exact", check_skip_fuzzy_if_exact);
	g_object_set_data(G_OBJECT(dialog), "check_remove_trailing_word_part",
		check_remove_trailing_word_part);
	g_signal_connect(dialog, "response", G_CALLBACK(configure_response_cb), NULL);

	gtk_widget_show_all(vbox);

	return vbox;
}


void plugin_cleanup(void)
{
	g_list_free_full(candidates, (GDestroyNotify)&free_candidate);
	free_plugin_config(plugin_config);
}


void plugin_help(void)
{
	utils_open_browser("http://plugins.geany.org/cycleautocomplete.html");
}

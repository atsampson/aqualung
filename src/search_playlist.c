/*                                                     -*- linux-c -*-
    Copyright (C) 2004 Tom Szilagyi

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    $Id$
*/

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include "common.h"
#include "playlist.h"
#include "gui_main.h"
#include "i18n.h"


extern GtkWidget* gui_stock_label_button(gchar *blabel, const gchar *bstock);

extern GtkListStore * play_store;
extern GtkWidget * play_list;

extern int playlist_is_embedded;
extern GtkWidget * playlist_window;
extern GtkWidget * main_window;

static GtkWidget * search_window = NULL;
static GtkWidget * searchkey_entry;

static GtkWidget * check_case;
static GtkWidget * check_exact;

static GtkListStore * search_store;
static GtkTreeSelection * search_select;


static void
clear_search_store(void) {
	int i;
	GtkTreeIter iter;

	i = 0;
	while (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(search_store), &iter, NULL, i++)) {

		gpointer gptr;
		GtkTreePath * path;

		gtk_tree_model_get(GTK_TREE_MODEL(search_store), &iter,
				   1, &gptr, -1);

		path = (GtkTreePath *)gptr;

		if (path != NULL) {
			gtk_tree_path_free(path);
		}
	}

	gtk_list_store_clear(search_store);
}


static gint
close_button_clicked(GtkWidget * widget, gpointer data) {

	clear_search_store();
        gtk_widget_destroy(search_window);
        search_window = NULL;
        return TRUE;
}


static int
search_window_close(GtkWidget * widget, gpointer * data) {

	clear_search_store();
        search_window = NULL;
        return 0;
}


static gint
search_button_clicked(GtkWidget * widget, gpointer data) {

	int casesens = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_case)) ? 1 : 0;
	int exactonly = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_exact)) ? 1 : 0;

	int valid;
	const char * key_string = gtk_entry_get_text(GTK_ENTRY(searchkey_entry));
	char key[MAXLEN];
	GPatternSpec * pattern;

	int i;
	GtkTreeIter list_iter;

	clear_search_store();

	valid = 0;
	for (i = 0; key_string[i] != '\0'; i++) {
		if ((key_string[i] != '?') && (key_string[i] != '*')) {
			valid = 1;
			break;
		}
	}
	if (!valid) {
		return TRUE;
	}

	if (!casesens) {
		key_string = g_utf8_strup(key_string, -1);
	}

	if (exactonly) {
		strcpy(key, key_string);
	} else {
		snprintf(key, MAXLEN-1, "*%s*", key_string);
	}

	pattern = g_pattern_spec_new(key);

	i = 0;
	while (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(play_store), &list_iter, NULL, i++)) {

		char * text;

		gtk_tree_model_get(GTK_TREE_MODEL(play_store), &list_iter, 0, &text, -1);

		char * tmp = NULL;
		if (casesens) {
			tmp = strdup(text);
		} else {
			tmp = g_utf8_strup(text, -1);
		}
		if (g_pattern_match_string(pattern, tmp)) {

			GtkTreeIter iter;
			GtkTreePath * path;

			path = gtk_tree_model_get_path(GTK_TREE_MODEL(play_store), &list_iter);
			gtk_list_store_append(search_store, &iter);
			gtk_list_store_set(search_store, &iter,
					   0, text,
					   1, (gpointer)path,
					   -1);
		}

		g_free(tmp);
		g_free(text);
		deflicker();
	}

	g_pattern_spec_free(pattern);

        return TRUE;
}


static void
search_selection_changed(GtkTreeSelection * treeselection, gpointer user_data) {

	GtkTreeIter iter;
	GtkTreePath * path;
	gpointer gptr;

	if (!gtk_tree_selection_get_selected(search_select, NULL, &iter)) {
		return;
	}

	gtk_tree_model_get(GTK_TREE_MODEL(search_store), &iter,
			   1, &gptr, -1);

	path = (GtkTreePath *)gptr;
	gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(play_list), path, NULL, TRUE, 0.5f, 0.5f);
	gtk_tree_view_set_cursor(GTK_TREE_VIEW(play_list), path, NULL, FALSE);
}


static gint
search_window_key_pressed(GtkWidget * widget, GdkEventKey * kevent) {

	switch (kevent->keyval) {

	case GDK_Escape:
		close_button_clicked(NULL, NULL);
		return TRUE;
		break;

	case GDK_Return:
	case GDK_KP_Enter:
		search_button_clicked(NULL, NULL);
		return TRUE;
		break;
	}

	return FALSE;
}


void
search_playlist_dialog(void) {

	GtkWidget * vbox;
	GtkWidget * hbox;
	GtkWidget * label;
	GtkWidget * button;
	GtkWidget * table;

        GtkWidget * search_viewport;
        GtkWidget * search_scrwin;
        GtkWidget * search_list;
        GtkCellRenderer * search_renderer;
        GtkTreeViewColumn * search_column;


        if (search_window != NULL) {
		return;
        }

        search_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(search_window), _("Search the Playlist"));
        gtk_window_set_position(GTK_WINDOW(search_window), GTK_WIN_POS_CENTER);


	if (playlist_is_embedded) {
		gtk_window_set_transient_for(GTK_WINDOW(search_window), GTK_WINDOW(main_window));
	} else {
		gtk_window_set_transient_for(GTK_WINDOW(search_window), GTK_WINDOW(playlist_window));
	}


	gtk_window_set_modal(GTK_WINDOW(search_window), TRUE);
        g_signal_connect(G_OBJECT(search_window), "delete_event",
                         G_CALLBACK(search_window_close), NULL);
        g_signal_connect(G_OBJECT(search_window), "key_press_event",
			 G_CALLBACK(search_window_key_pressed), NULL);
        gtk_container_set_border_width(GTK_CONTAINER(search_window), 5);

        vbox = gtk_vbox_new(FALSE, 0);
        gtk_container_add(GTK_CONTAINER(search_window), vbox);


	hbox = gtk_hbox_new(FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 3);

	label = gtk_label_new(_("Key: "));
        gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 5);

        searchkey_entry = gtk_entry_new();
        gtk_box_pack_start(GTK_BOX(hbox), searchkey_entry, TRUE, TRUE, 5);


	table = gtk_table_new(4, 2, FALSE);
        gtk_box_pack_start(GTK_BOX(vbox), table, FALSE, TRUE, 3);

	check_case = gtk_check_button_new_with_label(_("Case sensitive"));
	gtk_widget_set_name(check_case, "check_on_window");
	gtk_table_attach(GTK_TABLE(table), check_case, 0, 1, 0, 1,
			 GTK_EXPAND | GTK_FILL, GTK_FILL, 1, 4);

	check_exact = gtk_check_button_new_with_label(_("Exact matches only"));
	gtk_widget_set_name(check_exact, "check_on_window");
	gtk_table_attach(GTK_TABLE(table), check_exact, 1, 2, 0, 1,
			 GTK_EXPAND | GTK_FILL, GTK_FILL, 1, 4);


	hbox = gtk_hbox_new(FALSE, 0);
	gtk_container_set_border_width(GTK_CONTAINER(hbox), 3);
        gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 3);

	search_viewport = gtk_viewport_new(NULL, NULL);
        gtk_box_pack_start(GTK_BOX(hbox), search_viewport, TRUE, TRUE, 0);

        search_scrwin = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(search_scrwin),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(search_viewport), search_scrwin);


        search_store = gtk_list_store_new(2,
					  G_TYPE_STRING,   /* title */
					  G_TYPE_POINTER); /* * GtkTreePath */

        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(search_store), 0, GTK_SORT_ASCENDING);

        search_list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(search_store));
        gtk_widget_set_size_request(search_list, 400, 200);
        gtk_container_add(GTK_CONTAINER(search_scrwin), search_list);
        search_select = gtk_tree_view_get_selection(GTK_TREE_VIEW(search_list));
        gtk_tree_selection_set_mode(search_select, GTK_SELECTION_SINGLE);
        g_signal_connect(G_OBJECT(search_select), "changed",
                         G_CALLBACK(search_selection_changed), NULL);

        search_renderer = gtk_cell_renderer_text_new();
        search_column = gtk_tree_view_column_new_with_attributes(_("Title"),
                                                             search_renderer,
                                                             "text", 0,
                                                             NULL);
        gtk_tree_view_column_set_sizing(GTK_TREE_VIEW_COLUMN(search_column),
                                        GTK_TREE_VIEW_COLUMN_AUTOSIZE);
        gtk_tree_view_column_set_resizable(GTK_TREE_VIEW_COLUMN(search_column), TRUE);
        gtk_tree_view_column_set_sort_column_id(GTK_TREE_VIEW_COLUMN(search_column), 0);
        gtk_tree_view_append_column(GTK_TREE_VIEW(search_list), search_column);


	hbox = gtk_hbox_new(FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 3);

        button = gui_stock_label_button(_("Search"), GTK_STOCK_FIND);
        g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(search_button_clicked), NULL);
        gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 3);

        button = gtk_button_new_from_stock (GTK_STOCK_CLOSE); 
	g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(close_button_clicked), NULL);
        gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 3);

	gtk_widget_show_all(search_window);
}


// vim: shiftwidth=8:tabstop=8:softtabstop=8 :  

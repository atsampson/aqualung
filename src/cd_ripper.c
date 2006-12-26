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

#ifdef _WIN32
#include <glib.h>
#else
#include <pthread.h>
#endif /* _WIN32 */

#include "common.h"
#include "rb.h"
#include "cover.h"
#include "decoder/file_decoder.h"
#include "decoder/dec_cdda.h"
#include "encoder/file_encoder.h"
#include "encoder/enc_lame.h"
#include "gui_main.h"
#include "music_browser.h"
#include "options.h"
#include "i18n.h"
#include "cdda.h"
#include "cd_ripper.h"

#if defined(HAVE_CDDA) && (defined(HAVE_SNDFILE) || defined(HAVE_FLAC) || defined(HAVE_VORBISENC) || defined(HAVE_LAME))

extern options_t options;
extern GtkWidget * browser_window;
extern GtkTreeStore * music_store;

extern GdkPixbuf * icon_artist;
extern GdkPixbuf * icon_record;
extern GdkPixbuf * icon_track;

GtkListStore * ripper_source_store;
GtkWidget * ripper_dialog;
GtkWidget * ripper_artist_entry;
GtkWidget * ripper_album_entry;
GtkWidget * ripper_destdir_entry;
GtkWidget * ripper_deststore_combo;
GtkWidget * ripper_format_combo;
GtkWidget * ripper_bitrate_scale;
GtkWidget * ripper_bitrate_label;
GtkWidget * ripper_bitrate_value_label;
GtkWidget * ripper_vbr_check;
GtkWidget * ripper_meta_check;
GtkWidget * ripper_overlap_check;
GtkWidget * ripper_verify_check;
GtkWidget * ripper_neverskip_check;
GtkWidget * ripper_maxretries_spinner;
GtkWidget * ripper_maxretries_label;

GtkListStore * ripper_prog_store;
GtkWidget * ripper_prog_window;
GtkWidget * ripper_cancel_button;
GtkWidget * ripper_close_when_ready_check;
GtkWidget * ripper_hbox;

AQUALUNG_THREAD_DECLARE(ripper_thread_id)
int ripper_thread_busy;

int ripper_format;
int ripper_bitrate;
int ripper_vbr;
int ripper_meta;
char ripper_artist[MAXLEN];
char ripper_album[MAXLEN];
int ripper_write_to_store;
GtkTreeIter ripper_dest_store;
GtkTreeIter ripper_dest_artist;
GtkTreeIter ripper_dest_record;
int ripper_paranoia_mode;
int ripper_paranoia_maxretries;

int total_sectors;
char destdir[MAXLEN];


GtkWidget *
create_notebook_page(GtkWidget * nb, char * title) {

        GtkWidget * vbox = gtk_vbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(vbox), 5);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), vbox, gtk_label_new(title));
	return vbox;
}


GtkWidget *
create_frame_on_page(GtkWidget * vbox, char * title) {

	GtkWidget * vbox1;
	GtkWidget * frame = gtk_frame_new(title);
	gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 5);
        vbox1 = gtk_vbox_new(FALSE, 0);
        gtk_container_add(GTK_CONTAINER(frame), vbox1);
	return vbox1;
}


void
ripper_source_store_make(GtkTreeIter * record_iter) {

	GtkTreeIter track_iter;
	GtkTreeIter iter;
	int n = 0;

	while (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(music_store), &track_iter, record_iter, n++)) {

		char * track_name;
                gtk_tree_model_get(GTK_TREE_MODEL(music_store), &track_iter, 0, &track_name, -1);

		gtk_list_store_append(ripper_source_store, &iter);
		gtk_list_store_set(ripper_source_store, &iter,
				   0, TRUE, /* rip all tracks by default */
				   1, n,
				   2, track_name,
				   -1);

		g_free(track_name);
	}
}


void
ripper_cell_edited_cb(GtkCellRendererText * cell, gchar * path, gchar * text, gpointer data) {

        GtkTreeIter iter;

        if (gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(ripper_source_store), &iter, path)) {
                gtk_list_store_set(ripper_source_store, &iter, 2, text, -1);
        }
}


void
ripper_cell_toggled_cb(GtkCellRendererToggle * cell, gchar * path, gpointer data) {

        GtkTreeIter iter;

        if (gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(ripper_source_store), &iter, path)) {
		gboolean b;
		gtk_tree_model_get(GTK_TREE_MODEL(ripper_source_store), &iter, 0, &b, -1);
                gtk_list_store_set(ripper_source_store, &iter, 0, !b, -1);
        }
}


void
ripper_set_all_cb(GtkWidget * widget, gpointer data) {

	gboolean b = (gboolean)data;
	GtkTreeIter iter;
	int n = 0;
	while (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(ripper_source_store), &iter, NULL, n++)) {
                gtk_list_store_set(ripper_source_store, &iter, 0, b, -1);
	}
}


void
ripper_destdir_browse_cb(GtkWidget * widget, gpointer data) {

        GtkWidget * dialog;
	const gchar * selected_filename = gtk_entry_get_text(GTK_ENTRY(data));


        dialog = gtk_file_chooser_dialog_new(_("Please select the directory for ripped files."),
                                             GTK_WINDOW(ripper_dialog),
					     GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                             GTK_STOCK_APPLY, GTK_RESPONSE_ACCEPT,
                                             GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                             NULL);

	if (options.show_hidden) {
		gtk_file_chooser_set_show_hidden(GTK_FILE_CHOOSER(dialog), options.show_hidden);
	} 

        deflicker();

        if (strlen(selected_filename)) {
      		char * locale = g_locale_from_utf8(selected_filename, -1, NULL, NULL, NULL);
                char tmp[MAXLEN];
                tmp[0] = '\0';

		if (locale == NULL) {
			gtk_widget_destroy(dialog);
			return;
		}

                if (locale[0] == '~') {
                        snprintf(tmp, MAXLEN-1, "%s%s", options.home, locale + 1);
                        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), tmp);
                } else if (locale[0] == '/') {
                        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), locale);
                } else if (locale[0] != '\0') {
                        snprintf(tmp, MAXLEN-1, "%s/%s", options.cwd, locale + 1);
                        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), tmp);
                }

		g_free(locale);
	} else {
                gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), options.currdir);
	}

        gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
        gtk_window_set_default_size(GTK_WINDOW(dialog), 580, 390);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);


        if (aqualung_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {

		char * utf8;

                selected_filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		utf8 = g_locale_to_utf8(selected_filename, -1, NULL, NULL, NULL);

		if (utf8 == NULL) {
			gtk_widget_destroy(dialog);
		}

		gtk_entry_set_text(GTK_ENTRY(data), utf8);

                strncpy(options.currdir, selected_filename, MAXLEN-1);
		g_free(utf8);
        }

        gtk_widget_destroy(dialog);
}


GtkWidget *
create_ripper_deststore_combo(void) {

	GtkWidget * combo = gtk_combo_box_new_text();
	GtkTreeIter iter;
	int n;

	gtk_combo_box_append_text(GTK_COMBO_BOX(combo), _("(none)"));
	gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);

	n = 0;
	while (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(music_store), &iter, NULL, n++)) {

		char * name;
		float writable;
                gtk_tree_model_get(GTK_TREE_MODEL(music_store), &iter, 0, &name, 7, &writable, -1);

		if (is_store_iter_cdda(&iter)) {
			g_free(name);
			continue;
		}

		if (writable < 0.0f) { /* read-only or unreachable store */
			g_free(name);
			continue;
		}

		gtk_combo_box_append_text(GTK_COMBO_BOX(combo), name);
		g_free(name);
	}

	return combo;
}


/* ret: 0 - no store selected; 1 - store selected, iter set */
int
get_ripper_deststore_iter(GtkTreeIter * iter_store) {

	GtkTreeIter iter;
	int selected = gtk_combo_box_get_active(GTK_COMBO_BOX(ripper_deststore_combo));
	int i = 1, n = 0;

	if (selected == 0) {
		return 0;
	}

	while (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(music_store), &iter, NULL, n++)) {

		char * name;
		float writable;
                gtk_tree_model_get(GTK_TREE_MODEL(music_store), &iter, 0, &name, 7, &writable, -1);

		if (is_store_iter_cdda(&iter)) {
			g_free(name);
			continue;
		}

		if (writable < 0.0f) { /* read-only or unreachable store */
			g_free(name);
			continue;
		}

		if (i == selected) {
			*iter_store = iter;
			g_free(name);
			return 1;
		}

		++i;
		g_free(name);
	}
	return 0;
}


GtkWidget *
create_ripper_format_combo(void) {

	GtkWidget * combo = gtk_combo_box_new_text();
#if defined(HAVE_SNDFILE) || defined(HAVE_FLAC)
	int n = 0;
#endif /* HAVE_SNDFILE || HAVE_FLAC */

#ifdef HAVE_SNDFILE
	gtk_combo_box_append_text(GTK_COMBO_BOX(combo), "WAV");
	++n;
#endif /* HAVE_SNDFILE */
#ifdef HAVE_FLAC
	gtk_combo_box_append_text(GTK_COMBO_BOX(combo), "FLAC");
	gtk_combo_box_set_active(GTK_COMBO_BOX(combo), n);
#endif /* HAVE_FLAC */
#ifdef HAVE_VORBISENC
	gtk_combo_box_append_text(GTK_COMBO_BOX(combo), "Ogg Vorbis");
#endif /* HAVE_VORBISENC */
#ifdef HAVE_LAME
	gtk_combo_box_append_text(GTK_COMBO_BOX(combo), "MP3");
#endif /* HAVE_LAME */

	if (gtk_combo_box_get_active(GTK_COMBO_BOX(combo)) == -1) {
	    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
	}

	return combo;
}


/* returns file_lib value */
int
get_ripper_format(void) {

	int file_lib = -1;
	gchar * text = gtk_combo_box_get_active_text(GTK_COMBO_BOX(ripper_format_combo));
	if (strcmp(text, "WAV") == 0) {
		file_lib = ENC_SNDFILE_LIB;
	}
	if (strcmp(text, "FLAC") == 0) {
		file_lib = ENC_FLAC_LIB;
	}
	if (strcmp(text, "Ogg Vorbis") == 0) {
		file_lib = ENC_VORBIS_LIB;
	}
	if (strcmp(text, "MP3") == 0) {
		file_lib = ENC_LAME_LIB;
	}
	g_free(text);
	return file_lib;
}


void
ripper_bitrate_changed(GtkRange * range, gpointer data) {

	float val = gtk_range_get_value(range);
	gchar * text = gtk_combo_box_get_active_text(GTK_COMBO_BOX(ripper_format_combo));
	if (strcmp(text, "FLAC") == 0) {
		int i = (int)val;
		char str[256];
		switch (i) {
		case 0:
		case 8:
			snprintf(str, 255, "%d (%s)", i, (i == 0) ? _("fast") : _("best"));
			gtk_label_set_text(GTK_LABEL(ripper_bitrate_value_label), str);
			break;
		default:
			snprintf(str, 255, "%d", i);
			gtk_label_set_text(GTK_LABEL(ripper_bitrate_value_label), str);
			break;
		}
	}
	if (strcmp(text, "Ogg Vorbis") == 0) {
		int i = (int)val;
		char str[256];
		snprintf(str, 255, "%d", i);
		gtk_label_set_text(GTK_LABEL(ripper_bitrate_value_label), str);
	}
	if (strcmp(text, "MP3") == 0) {
		int i = (int)val;
		char str[256];
#ifdef HAVE_LAME
		i = lame_encoder_validate_bitrate(i, 0);
#endif /* HAVE_LAME */
		snprintf(str, 255, "%d", i);
		gtk_label_set_text(GTK_LABEL(ripper_bitrate_value_label), str);
	}
	g_free(text);
}


void
ripper_format_combo_changed(GtkWidget * widget, gpointer data) {

	gchar * text = gtk_combo_box_get_active_text(GTK_COMBO_BOX(widget));

	if (strcmp(text, "WAV") == 0) {
		gtk_widget_hide(ripper_bitrate_scale);
		gtk_widget_hide(ripper_bitrate_label);
		gtk_widget_hide(ripper_bitrate_value_label);
		gtk_widget_hide(ripper_vbr_check);
		gtk_widget_hide(ripper_meta_check);
	}
	if (strcmp(text, "FLAC") == 0) {
		gtk_widget_show(ripper_bitrate_scale);
		gtk_widget_show(ripper_bitrate_label);
		gtk_label_set_text(GTK_LABEL(ripper_bitrate_label), _("Compression level:"));
		gtk_widget_show(ripper_bitrate_value_label);
		gtk_widget_hide(ripper_vbr_check);
		gtk_widget_show(ripper_meta_check);

		gtk_range_set_range(GTK_RANGE(ripper_bitrate_scale), 0, 8);
		gtk_range_set_value(GTK_RANGE(ripper_bitrate_scale), 5);
	}
	if (strcmp(text, "Ogg Vorbis") == 0) {
		gtk_widget_show(ripper_bitrate_scale);
		gtk_widget_show(ripper_bitrate_label);
		gtk_label_set_text(GTK_LABEL(ripper_bitrate_label), _("Bitrate [kbps]:"));
		gtk_widget_show(ripper_bitrate_value_label);
		gtk_widget_hide(ripper_vbr_check);
		gtk_widget_show(ripper_meta_check);

		gtk_range_set_range(GTK_RANGE(ripper_bitrate_scale), 32, 320);
		gtk_range_set_value(GTK_RANGE(ripper_bitrate_scale), 256);
	}
	if (strcmp(text, "MP3") == 0) {
		gtk_widget_show(ripper_bitrate_scale);
		gtk_widget_show(ripper_bitrate_label);
		gtk_label_set_text(GTK_LABEL(ripper_bitrate_label), _("Bitrate [kbps]:"));
		gtk_widget_show(ripper_bitrate_value_label);
		gtk_widget_show(ripper_vbr_check);
		gtk_widget_show(ripper_meta_check);

		gtk_range_set_range(GTK_RANGE(ripper_bitrate_scale), 32, 320);
		gtk_range_set_value(GTK_RANGE(ripper_bitrate_scale), 256);
	}

	g_free(text);
}


void
ripper_paranoia_toggled(GtkWidget * widget, gpointer * data) {

	gtk_widget_set_sensitive(ripper_maxretries_spinner,
				 !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ripper_neverskip_check)));
	gtk_widget_set_sensitive(ripper_maxretries_label,
				 !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ripper_neverskip_check)));
}


int
ripper_handle_existing_record_iter(GtkTreeIter * iter) {

	GtkWidget * error_dialog;
	int n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(music_store), iter);
	if (n == 0)
		return 0;

	error_dialog = gtk_message_dialog_new(GTK_WINDOW(ripper_dialog),
					      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
					      GTK_MESSAGE_WARNING, GTK_BUTTONS_OK_CANCEL,
					      _("\nThe Music Store you selected has a matching Artist and "
						"Album, already containing some tracks. If you press OK, "
						"these tracks will be removed. The files themselves will "
						"be left intact, but they will be removed from the "
						"destination Music Store. Press Cancel to get back to "
						"change the Artist/Album or the destination Music Store."));
	
	gtk_window_set_title(GTK_WINDOW(error_dialog), _("Artist/Album already existing, not empty"));
	gtk_widget_show(error_dialog);			
	if (aqualung_dialog_run(GTK_DIALOG(error_dialog)) == GTK_RESPONSE_OK) {

		GtkTreeIter track_iter;
		gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(music_store), &track_iter, iter, 0);
		while (gtk_tree_store_remove(music_store, &track_iter));
		music_store_mark_changed(iter);
		
		gtk_widget_destroy(error_dialog);
		return 0;
	}
	gtk_widget_destroy(error_dialog);
	return 1;
}


/* ret: 0 - ok, 1 - already found, user bailed out of overwriting */
int
ripper_make_dest_iters(GtkTreeIter * store_iter,
		       GtkTreeIter * artist_iter,
		       GtkTreeIter * record_iter) {

        int i;
        int j;

        i = 0;
        while (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(music_store),
                                             artist_iter, store_iter, i++)) {
                char * artist_name;
                gtk_tree_model_get(GTK_TREE_MODEL(music_store), artist_iter,
                                   0, &artist_name, -1);

                if (g_utf8_collate(ripper_artist, artist_name)) {
                        g_free(artist_name);
                        continue;
                }

                j = 0;
                while (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(music_store),
                                                     record_iter, artist_iter, j++)) {
                        char * record_name;
                        gtk_tree_model_get(GTK_TREE_MODEL(music_store), record_iter,
                                           0, &record_name, -1);

                        if (!g_utf8_collate(ripper_album, record_name)) {
				int ret = ripper_handle_existing_record_iter(record_iter);
                                g_free(record_name);
                                g_free(artist_name);
                                return ret;
                        }
                        g_free(record_name);
                }
                /* create record */
                gtk_tree_store_append(music_store, record_iter, artist_iter);
                gtk_tree_store_set(music_store, record_iter,
                                   0, ripper_album, 1, ripper_album,
                                   2, "", 3, "", -1);
                if (options.enable_ms_tree_icons) {
                        gtk_tree_store_set(music_store, record_iter, 9, icon_record, -1);
                }
		music_store_mark_changed(record_iter);

                g_free(artist_name);
                return 0;
        }

        /* create both artist and record */
        gtk_tree_store_append(music_store, artist_iter, store_iter);
        gtk_tree_store_set(music_store, artist_iter,
                           0, ripper_artist, 1, ripper_artist, 2, "", 3, "", -1);
        if (options.enable_ms_tree_icons) {
                gtk_tree_store_set(music_store, artist_iter, 9, icon_artist, -1);
        }

        gtk_tree_store_append(music_store, record_iter, artist_iter);
        gtk_tree_store_set(music_store, record_iter,
                           0, ripper_album, 1, ripper_album,
                           2, "", 3, "", -1);
        if (options.enable_ms_tree_icons) {
                gtk_tree_store_set(music_store, record_iter, 9, icon_record, -1);
        }
	music_store_mark_changed(record_iter);

	return 0;
}


int
cd_ripper_dialog(char * device_path, GtkTreeIter * iter) {

        GtkWidget * notebook;
        GtkWidget * table;
        GtkWidget * hbox;
        GtkWidget * button;
        GtkWidget * frame;

	GtkWidget * vbox_source;
	GtkWidget * vbox_dest;
	GtkWidget * vbox_dest1;
	GtkWidget * vbox_format;
	GtkWidget * vbox_para;
	GtkWidget * vbox_para1;

	GtkWidget * source_tree;
	GtkWidget * viewport;
	GtkWidget * scrolled_win;
	GtkCellRenderer * cell;
	GtkTreeViewColumn * column;

	cdda_drive_t * drive = cdda_get_drive_by_device_path(device_path);
	if (drive == NULL)
		return 0;

        ripper_dialog = gtk_dialog_new_with_buttons(_("Rip CD"),
                                             GTK_WINDOW(browser_window),
                                             GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_NO_SEPARATOR,
                                             GTK_STOCK_OK, GTK_RESPONSE_ACCEPT,
                                             GTK_STOCK_CANCEL, GTK_RESPONSE_REJECT,
                                             NULL);

        gtk_window_set_position(GTK_WINDOW(ripper_dialog), GTK_WIN_POS_CENTER);
        gtk_window_set_default_size(GTK_WINDOW(ripper_dialog), 400, -1);
        gtk_dialog_set_default_response(GTK_DIALOG(ripper_dialog), GTK_RESPONSE_REJECT);

        notebook = gtk_notebook_new();
        gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_TOP);
        gtk_container_add(GTK_CONTAINER(GTK_DIALOG(ripper_dialog)->vbox), notebook);


        /* Source selection */

	vbox_source = create_notebook_page(notebook, _("Source"));

        table = gtk_table_new(4, 2, FALSE);
        gtk_box_pack_start(GTK_BOX(vbox_source), table, FALSE, FALSE, 0);

        hbox = gtk_hbox_new(FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(_("Artist:")), FALSE, FALSE, 0);
        gtk_table_attach(GTK_TABLE(table), hbox, 0, 1, 0, 1, GTK_FILL, GTK_FILL, 5, 5);

        ripper_artist_entry = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(ripper_artist_entry), MAXLEN-1);
	gtk_entry_set_text(GTK_ENTRY(ripper_artist_entry), drive->disc.artist_name);
        gtk_table_attach(GTK_TABLE(table), ripper_artist_entry, 1, 2, 0, 1,
                         GTK_EXPAND | GTK_FILL, GTK_FILL, 5, 5);

        hbox = gtk_hbox_new(FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(_("Album:")), FALSE, FALSE, 0);
        gtk_table_attach(GTK_TABLE(table), hbox, 0, 1, 1, 2, GTK_FILL, GTK_FILL, 5, 5);

        ripper_album_entry = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(ripper_album_entry), MAXLEN-1);
	gtk_entry_set_text(GTK_ENTRY(ripper_album_entry), drive->disc.record_name);
        gtk_table_attach(GTK_TABLE(table), ripper_album_entry, 1, 2, 1, 2,
                         GTK_EXPAND | GTK_FILL, GTK_FILL, 5, 5);


	if (ripper_source_store == NULL) {
		ripper_source_store = gtk_list_store_new(3,
							 G_TYPE_BOOLEAN, /* rip this track? */
							 G_TYPE_INT,     /* track number */
							 G_TYPE_STRING); /* track name */
	} else {
		gtk_list_store_clear(ripper_source_store);
	}
	ripper_source_store_make(iter);

        viewport = gtk_viewport_new(NULL, NULL);
        gtk_table_attach(GTK_TABLE(table), viewport, 0, 2, 2, 3,
                         GTK_EXPAND | GTK_FILL, GTK_EXPAND | GTK_FILL, 5, 5);
        scrolled_win = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_win),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(viewport), scrolled_win);
	source_tree = gtk_tree_view_new();
	gtk_tree_view_set_model(GTK_TREE_VIEW(source_tree), GTK_TREE_MODEL(ripper_source_store));
        gtk_widget_set_size_request(source_tree, -1, 300);
        gtk_container_add(GTK_CONTAINER(scrolled_win), source_tree);

	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(source_tree), FALSE);
        if (options.enable_ms_rules_hint) {
                gtk_tree_view_set_rules_hint(GTK_TREE_VIEW(source_tree), TRUE);
        }

        cell = gtk_cell_renderer_toggle_new();
        g_object_set(cell, "activatable", TRUE, NULL);
        g_signal_connect(cell, "toggled", (GCallback)ripper_cell_toggled_cb, NULL);
        column = gtk_tree_view_column_new_with_attributes(_("Rip"), cell, "active", 0, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(source_tree), GTK_TREE_VIEW_COLUMN(column));

        cell = gtk_cell_renderer_text_new();
        column = gtk_tree_view_column_new_with_attributes(_("No"), cell, "text", 1, NULL);

        gtk_tree_view_append_column(GTK_TREE_VIEW(source_tree), GTK_TREE_VIEW_COLUMN(column));
        cell = gtk_cell_renderer_text_new();
        g_object_set(cell, "editable", TRUE, NULL);
        g_signal_connect(cell, "edited", (GCallback)ripper_cell_edited_cb, NULL);
        column = gtk_tree_view_column_new_with_attributes(_("Title"), cell, "text", 2, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(source_tree), GTK_TREE_VIEW_COLUMN(column));


        hbox = gtk_hbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(hbox), 2);
        gtk_table_attach(GTK_TABLE(table), hbox, 0, 2, 3, 4, GTK_EXPAND | GTK_FILL, GTK_FILL, 5, 5);

        gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(_("Select")), FALSE, FALSE, 5);

        button = gtk_button_new_with_label(_("All"));
        g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(ripper_set_all_cb), (gpointer)TRUE);
        gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 5);

        button = gtk_button_new_with_label(_("None"));
        g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(ripper_set_all_cb), (gpointer)FALSE);
        gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 5);


        /* Output */

	vbox_dest = create_notebook_page(notebook, _("Output"));
	vbox_dest1 = create_frame_on_page(vbox_dest, _("Destination"));

	frame = gtk_frame_new(_("Target directory for ripped files"));
	gtk_box_pack_start(GTK_BOX(vbox_dest1), frame, FALSE, FALSE, 2);
        gtk_container_set_border_width(GTK_CONTAINER(frame), 5);
        hbox = gtk_hbox_new(FALSE, 0);
        gtk_container_set_border_width(GTK_CONTAINER(hbox), 5);
        gtk_container_add(GTK_CONTAINER(frame), hbox);

        ripper_destdir_entry = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(ripper_destdir_entry), MAXLEN-1);
	gtk_box_pack_start(GTK_BOX(hbox), ripper_destdir_entry, TRUE, TRUE, 5);

        button = gui_stock_label_button(_("_Browse..."), GTK_STOCK_OPEN);
        g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(ripper_destdir_browse_cb),
			 (gpointer)ripper_destdir_entry);
        gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 4);

        hbox = gtk_hbox_new(FALSE, 0);
	gtk_box_pack_start(GTK_BOX(vbox_dest1), hbox, FALSE, FALSE, 5);

	gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(_("Add to Music Store")), FALSE, FALSE, 7);
	ripper_deststore_combo = create_ripper_deststore_combo();
	gtk_box_pack_start(GTK_BOX(hbox), ripper_deststore_combo, TRUE, TRUE, 5);
	

	vbox_format = create_frame_on_page(vbox_dest, _("Format"));

        table = gtk_table_new(4, 2, TRUE);
        gtk_box_pack_start(GTK_BOX(vbox_format), table, TRUE, TRUE, 0);

        hbox = gtk_hbox_new(FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(_("File format:")), FALSE, FALSE, 0);
        gtk_table_attach(GTK_TABLE(table), hbox, 0, 1, 0, 1, GTK_FILL, GTK_FILL, 5, 4);

	ripper_format_combo = create_ripper_format_combo();
        gtk_table_attach(GTK_TABLE(table), ripper_format_combo, 1, 2, 0, 1,
			 GTK_EXPAND | GTK_FILL, GTK_FILL, 5, 2);

        hbox = gtk_hbox_new(FALSE, 0);
	ripper_bitrate_label = gtk_label_new(_("Compression level:"));
        gtk_box_pack_start(GTK_BOX(hbox), ripper_bitrate_label, FALSE, FALSE, 0);
        gtk_table_attach(GTK_TABLE(table), hbox, 0, 1, 1, 2, GTK_FILL, GTK_FILL, 5, 0);

	ripper_bitrate_scale = gtk_hscale_new_with_range(0, 8, 1);
        g_signal_connect(G_OBJECT(ripper_bitrate_scale), "value-changed",
			 G_CALLBACK(ripper_bitrate_changed), NULL);
	gtk_scale_set_draw_value(GTK_SCALE(ripper_bitrate_scale), FALSE);
	gtk_scale_set_digits(GTK_SCALE(ripper_bitrate_scale), 0);
        gtk_widget_set_size_request(ripper_bitrate_scale, 180, -1);
        gtk_table_attach(GTK_TABLE(table), ripper_bitrate_scale, 1, 2, 1, 2,
			 GTK_EXPAND | GTK_FILL, GTK_FILL, 5, 0);

	ripper_bitrate_value_label = gtk_label_new("0 (fast)");
        gtk_table_attach(GTK_TABLE(table), ripper_bitrate_value_label, 1, 2, 2, 3,
			 GTK_EXPAND | GTK_FILL, GTK_FILL, 5, 0);

        ripper_vbr_check = gtk_check_button_new_with_label(_("VBR encoding"));
        gtk_widget_set_name(ripper_vbr_check, "check_on_notebook");
        gtk_table_attach(GTK_TABLE(table), ripper_vbr_check, 0, 1, 2, 3, GTK_FILL, GTK_FILL, 5, 0);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ripper_vbr_check), TRUE);

        ripper_meta_check = gtk_check_button_new_with_label(_("Tag files with metadata"));
        gtk_widget_set_name(ripper_meta_check, "check_on_notebook");
        gtk_table_attach(GTK_TABLE(table), ripper_meta_check, 0, 2, 3, 4,
			 GTK_EXPAND | GTK_FILL, GTK_FILL, 5, 4);

        g_signal_connect(G_OBJECT(ripper_format_combo), "changed",
			 G_CALLBACK(ripper_format_combo_changed), NULL);


        /* Paranoia */

	vbox_para = create_notebook_page(notebook, _("Paranoia"));
	vbox_para1 = create_frame_on_page(vbox_para, _("Paranoia error correction"));
        gtk_container_set_border_width(GTK_CONTAINER(vbox_para1), 5);

        ripper_overlap_check = gtk_check_button_new_with_label(_("Perform overlapped reads"));
        gtk_widget_set_name(ripper_overlap_check, "check_on_notebook");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ripper_overlap_check), TRUE);
        gtk_box_pack_start(GTK_BOX(vbox_para1), ripper_overlap_check, FALSE, FALSE, 3);

        ripper_verify_check = gtk_check_button_new_with_label(_("Verify data integrity"));
        gtk_widget_set_name(ripper_verify_check, "check_on_notebook");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ripper_verify_check), TRUE);
        gtk_box_pack_start(GTK_BOX(vbox_para1), ripper_verify_check, FALSE, FALSE, 3);

        ripper_neverskip_check = gtk_check_button_new_with_label(_("Unlimited retry on failed reads (never skip)"));
        gtk_widget_set_name(ripper_neverskip_check, "check_on_notebook");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ripper_neverskip_check), TRUE);
        gtk_box_pack_start(GTK_BOX(vbox_para1), ripper_neverskip_check, FALSE, FALSE, 3);
        g_signal_connect(ripper_neverskip_check, "toggled", G_CALLBACK(ripper_paranoia_toggled), NULL);

        hbox = gtk_hbox_new(FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox_para1), hbox, FALSE, FALSE, 3);

	ripper_maxretries_label = gtk_label_new(_("\tMaximum number of retries:"));
	gtk_widget_set_sensitive(ripper_maxretries_label, FALSE);
        gtk_box_pack_start(GTK_BOX(hbox), ripper_maxretries_label, FALSE, FALSE, 5);

        ripper_maxretries_spinner = gtk_spin_button_new_with_range(1, 50, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ripper_maxretries_spinner), options.cdda_paranoia_maxretries);
	gtk_widget_set_sensitive(ripper_maxretries_spinner, FALSE);
        gtk_box_pack_start(GTK_BOX(hbox), ripper_maxretries_spinner, FALSE, FALSE, 5);


        gtk_widget_show_all(ripper_dialog);
	ripper_format_combo_changed(ripper_format_combo, NULL);

 ripper_display:
	destdir[0] = '\0';
        if (aqualung_dialog_run(GTK_DIALOG(ripper_dialog)) == GTK_RESPONSE_ACCEPT) {

		char * pdestdir = g_locale_from_utf8(gtk_entry_get_text(GTK_ENTRY(ripper_destdir_entry)), -1, NULL, NULL, NULL);

                if ((pdestdir == NULL) || (pdestdir[0] == '\0')) {
                        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 1);
                        gtk_widget_grab_focus(ripper_destdir_entry);
			g_free(pdestdir);
                        goto ripper_display;
                }

                if (pdestdir[0] == '~') {
                        snprintf(destdir, MAXLEN-1, "%s%s", options.home, pdestdir + 1);
                } else if (pdestdir[0] == '/') {
                        strncpy(destdir, pdestdir, MAXLEN-1);
                } else if (pdestdir[0] != '\0') {
                        snprintf(destdir, MAXLEN-1, "%s/%s", options.cwd, pdestdir);
                }
		g_free(pdestdir);

		if (access(destdir, R_OK | W_OK) != 0) {
			GtkWidget * error_dialog =
				gtk_message_dialog_new(GTK_WINDOW(ripper_dialog),
						       GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
						       GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
						       _("\nDestination directory is not read-write accessible!"));

			gtk_window_set_title(GTK_WINDOW(error_dialog), _("Error"));
			gtk_widget_show(error_dialog);			
			aqualung_dialog_run(GTK_DIALOG(error_dialog));
			gtk_widget_destroy(error_dialog);
			
                        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook), 1);
                        gtk_widget_grab_focus(ripper_destdir_entry);
			goto ripper_display;
		}

		ripper_format = get_ripper_format();
		ripper_bitrate = gtk_range_get_value(GTK_RANGE(ripper_bitrate_scale));
		ripper_vbr = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ripper_vbr_check));
		ripper_meta = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ripper_meta_check));
		strncpy(ripper_artist, gtk_entry_get_text(GTK_ENTRY(ripper_artist_entry)), MAXLEN-1);
		strncpy(ripper_album, gtk_entry_get_text(GTK_ENTRY(ripper_album_entry)), MAXLEN-1);
		ripper_write_to_store = get_ripper_deststore_iter(&ripper_dest_store);

		if (ripper_write_to_store) {
			if (ripper_make_dest_iters(&ripper_dest_store,
						   &ripper_dest_artist,
						   &ripper_dest_record) == 1) {

				goto ripper_display;
			}
		}

		ripper_paranoia_mode =
			(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ripper_overlap_check)) ? PARANOIA_MODE_OVERLAP : 0) |
			(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ripper_verify_check)) ? PARANOIA_MODE_VERIFY : 0) |
			(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ripper_neverskip_check)) ? PARANOIA_MODE_NEVERSKIP : 0);
		ripper_paranoia_maxretries = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ripper_maxretries_spinner));

		gtk_widget_destroy(ripper_dialog);
		return 1;
	} else {
		gtk_widget_destroy(ripper_dialog);
		return 0;
	}
}


void
sector_to_str(int sector, char * str) {

	int m, s, f;

	m = sector / (60*75);
	s = sector / 75 - m * 60;
	f = sector % 75;

	snprintf(str, MAXLEN-1, "%d [%02d:%02d.%02d]", sector, m, s, f);
}


void
ripper_prog_store_make(char * device_path) {

	GtkTreeIter source_iter;
	GtkTreeIter iter;
	int n = 0;
	char begin[MAXLEN];
	char length[MAXLEN];

	cdda_drive_t * drive = cdda_get_drive_by_device_path(device_path);
	if (drive == NULL)
		return;

	total_sectors = 0;
	while (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(ripper_source_store), &source_iter, NULL, n++)) {

		gboolean b;
		int len;
		char num[16];
                gtk_tree_model_get(GTK_TREE_MODEL(ripper_source_store), &source_iter, 0, &b, -1);

		if (!b)
			continue;

		len = drive->disc.toc[n] - drive->disc.toc[n-1];
		total_sectors += len;
		sector_to_str(drive->disc.toc[n-1], begin);
		sector_to_str(len, length);
		sprintf(num, "%d.", n);

		gtk_list_store_append(ripper_prog_store, &iter);
		gtk_list_store_set(ripper_prog_store, &iter,
				   0, num,
				   1, begin,
				   2, length,
				   3, 0,
				   -1);
	}

	sector_to_str(total_sectors, length);
	
	gtk_list_store_append(ripper_prog_store, &iter);
	gtk_list_store_set(ripper_prog_store, &iter,
			   0, _("Total"),
			   1, _("(audio only)"),
			   2, length,
			   3, 0,
			   -1);
}


void
ripper_prog_window_close(GtkWidget * widget, gpointer data) {

	ripper_thread_busy = 0;
	gtk_widget_destroy(ripper_prog_window);
	ripper_prog_window = NULL;
	gtk_list_store_clear(ripper_source_store);
	gtk_list_store_clear(ripper_prog_store);
}


void
ripper_cancel(GtkWidget * widget, gpointer data) {

        ripper_prog_window_close(NULL, NULL);
}


void
ripper_window(void) {

	GtkWidget * vbox;
	GtkWidget * viewport;
	GtkWidget * scrolled_win;
	GtkWidget * prog_tree;
	GtkCellRenderer * cell;
	GtkTreeViewColumn * column;
	GtkWidget * hseparator;

        ripper_prog_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(ripper_prog_window), _("Ripping CD tracks..."));
        gtk_window_set_position(GTK_WINDOW(ripper_prog_window), GTK_WIN_POS_CENTER);
        g_signal_connect(G_OBJECT(ripper_prog_window), "delete_event",
                         G_CALLBACK(ripper_prog_window_close), NULL);
        gtk_container_set_border_width(GTK_CONTAINER(ripper_prog_window), 5);

        vbox = gtk_vbox_new(FALSE, 0);
        gtk_container_add(GTK_CONTAINER(ripper_prog_window), vbox);

        viewport = gtk_viewport_new(NULL, NULL);
        gtk_box_pack_start(GTK_BOX(vbox), viewport, TRUE, TRUE, 0);

        scrolled_win = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_win),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(viewport), scrolled_win);
	prog_tree = gtk_tree_view_new();
	gtk_tree_view_set_model(GTK_TREE_VIEW(prog_tree), GTK_TREE_MODEL(ripper_prog_store));
        gtk_widget_set_size_request(prog_tree, 500, 320);
        gtk_container_add(GTK_CONTAINER(scrolled_win), prog_tree);

	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(prog_tree), FALSE);
	gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(prog_tree)), GTK_SELECTION_NONE);
        if (options.enable_ms_rules_hint) {
                gtk_tree_view_set_rules_hint(GTK_TREE_VIEW(prog_tree), TRUE);
        }

        cell = gtk_cell_renderer_text_new();
        column = gtk_tree_view_column_new_with_attributes(_("No"), cell, "text", 0, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(prog_tree), GTK_TREE_VIEW_COLUMN(column));

        cell = gtk_cell_renderer_text_new();
	g_object_set((gpointer)cell, "xalign", 1.0, NULL);
        column = gtk_tree_view_column_new_with_attributes(_("Begin"), cell, "text", 1, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(prog_tree), GTK_TREE_VIEW_COLUMN(column));

        cell = gtk_cell_renderer_text_new();
	g_object_set((gpointer)cell, "xalign", 1.0, NULL);
        column = gtk_tree_view_column_new_with_attributes(_("Length"), cell, "text", 2, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(prog_tree), GTK_TREE_VIEW_COLUMN(column));

        cell = gtk_cell_renderer_progress_new();
        column = gtk_tree_view_column_new_with_attributes(_("Progress"), cell, "value", 3, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(prog_tree), GTK_TREE_VIEW_COLUMN(column));

        hseparator = gtk_hseparator_new();
        gtk_box_pack_start(GTK_BOX(vbox), hseparator, FALSE, TRUE, 5);

        ripper_hbox = gtk_hbox_new(FALSE, 0);
        gtk_box_pack_end(GTK_BOX(vbox), ripper_hbox, FALSE, TRUE, 0);

	ripper_close_when_ready_check = gtk_check_button_new_with_label(_("Close window when complete"));
        gtk_box_pack_start(GTK_BOX(ripper_hbox), ripper_close_when_ready_check, FALSE, TRUE, 0);

        ripper_cancel_button = gui_stock_label_button (_("Abort"), GTK_STOCK_CANCEL);
        g_signal_connect(ripper_cancel_button, "clicked", G_CALLBACK(ripper_cancel), NULL);
        gtk_box_pack_end(GTK_BOX(ripper_hbox), ripper_cancel_button, FALSE, TRUE, 0);

        gtk_widget_grab_focus(ripper_cancel_button);

	gtk_widget_show_all(ripper_prog_window);
}


gboolean
ripper_update_status(gpointer pdata) {

	GtkTreeIter iter;
	int data = (int)pdata;
	int track_no = (data & 0xff0000) >> 16;
	int prog_track = (data & 0xff00) >> 8;
	int prog_total = data & 0xff;
	int n_children = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(ripper_prog_store), NULL);

	if (!ripper_prog_window)
		return FALSE;

	if (prog_track > 100)
		prog_track = 100;
	if (prog_total > 100)
		prog_total = 100;

	if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(ripper_prog_store), &iter, NULL, track_no)) {
		gtk_list_store_set(ripper_prog_store, &iter, 3, prog_track, -1);
	}
	if (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(ripper_prog_store), &iter, NULL, n_children-1)) {
		gtk_list_store_set(ripper_prog_store, &iter, 3, prog_total, -1);
	}

	if (prog_total == 100) {
		if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ripper_close_when_ready_check))) {
			ripper_prog_window_close(NULL, NULL);
		} else {
			gtk_widget_destroy(ripper_cancel_button);
			ripper_cancel_button = gui_stock_label_button (_("Close"), GTK_STOCK_CLOSE);
			g_signal_connect(ripper_cancel_button, "clicked", G_CALLBACK(ripper_cancel), NULL);
			gtk_box_pack_end(GTK_BOX(ripper_hbox), ripper_cancel_button, FALSE, TRUE, 0);
			gtk_widget_grab_focus(ripper_cancel_button);
		}
	}

	return FALSE;
}


void *
ripper_thread(void * arg) {

	char * device_path = (char *)arg;
	cdda_drive_t * drive;
	long hash;
	int n = 0;
	int track_cnt = 0;
	int total_sectors_read = 0;
	GtkTreeIter source_iter;

        AQUALUNG_THREAD_DETACH()

        drive = cdda_get_drive_by_device_path(device_path);
	if (drive == NULL) {
		ripper_thread_busy = 0;
		free(device_path);
		return NULL;
	}
	hash = calc_cdda_hash(&drive->disc);

	while (gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(ripper_source_store), &source_iter, NULL, n++)) {

		gboolean b;
		int no;
		char * name;
		char * ext = "raw";
		char decoder_filename[256];
		int track_sectors = 0;
		int track_sectors_read = 0;

		file_decoder_t * fdec;
		file_encoder_t * fenc;
		encoder_mode_t mode;

                gtk_tree_model_get(GTK_TREE_MODEL(ripper_source_store), &source_iter, 0, &b, -1);

		if (!b)
			continue;

                gtk_tree_model_get(GTK_TREE_MODEL(ripper_source_store), &source_iter,
				   1, &no, 2, &name, -1);

		if (ripper_thread_busy == 0) {
			free(device_path);
			return NULL;
		}

		track_sectors = drive->disc.toc[no] - drive->disc.toc[no-1];

		switch (ripper_format) {
		case ENC_SNDFILE_LIB: ext = "wav"; break;
		case ENC_FLAC_LIB:    ext = "flac"; break;
		case ENC_VORBIS_LIB:  ext = "ogg"; break;
		case ENC_LAME_LIB:    ext = "mp3"; break;
		}

		snprintf(decoder_filename, 255, "CDDA %s %lX %d", device_path, hash, no);
		snprintf(mode.filename, MAXLEN-1, "%s/track%02d.%s", destdir, no, ext);
		mode.file_lib = ripper_format;
		mode.sample_rate = 44100;
		mode.channels = 2;
		if (mode.file_lib == ENC_FLAC_LIB) {
			mode.clevel = ripper_bitrate;
		} else if (mode.file_lib == ENC_VORBIS_LIB) {
			mode.bps = ripper_bitrate * 1000;
		} else if (mode.file_lib == ENC_LAME_LIB) {
			mode.bps = ripper_bitrate * 1000;
			mode.vbr = ripper_vbr;
		}

		fdec = file_decoder_new();
		fenc = file_encoder_new();
		file_decoder_open(fdec, decoder_filename);
		file_encoder_open(fenc, &mode);

		cdda_decoder_set_mode(((decoder_t *)fdec->pdec),
				      100, /* max drive speed */
				      ripper_paranoia_mode,
				      ripper_paranoia_maxretries);

		while (ripper_thread_busy) {
			float buf[2*588];
			int n_read;

			int prog_track;
			int prog_total;

			n_read = file_decoder_read(fdec, buf, 588);
			file_encoder_write(fenc, buf, n_read);

			++track_sectors_read;
			++total_sectors_read;

			prog_track = 100 * track_sectors_read / track_sectors;
			prog_total = 100 * total_sectors_read / total_sectors;

		        if ((track_sectors_read % 64 == 0) || (track_sectors_read == track_sectors))
				g_idle_add(ripper_update_status,
					   (gpointer)(((track_cnt & 0xff) << 16) |
						      ((prog_track & 0xff) << 8) |
						      (prog_total & 0xff)));

			if ((track_sectors_read >= track_sectors) || (n_read < 588))
				break;
		}

		if (ripper_write_to_store) {
			GtkTreeIter iter;
			char sort_name[3];
			snprintf(sort_name, 3, "%02d", no);
			gtk_tree_store_append(music_store, &iter, &ripper_dest_record);
			gtk_tree_store_set(music_store, &iter,
					   0, name,
					   1, sort_name,
					   2, mode.filename,
					   3, "",
					   4, track_sectors_read / 75.0,
					   5, 1.0f,
					   6, 0.0f,
					   7, -1.0f,
					   -1);
			if (options.enable_ms_tree_icons) {
				gtk_tree_store_set(music_store, &iter, 9, icon_track, -1);
			}
			music_store_mark_changed(&iter);
		}

		file_decoder_close(fdec);
		file_encoder_close(fenc);
		file_decoder_delete(fdec);
		file_encoder_delete(fenc);

		++track_cnt;
	}

	free(device_path);
	return NULL;
}


void
cd_ripper(char * device_path, GtkTreeIter * iter) {

	if (cd_ripper_dialog(device_path, iter)) {

		if (ripper_prog_store == NULL) {
			ripper_prog_store = gtk_list_store_new(4,
							       G_TYPE_STRING,  /* track number */
							       G_TYPE_STRING,  /* begin sector */
							       G_TYPE_STRING,  /* length (sectors) */
							       G_TYPE_INT);    /* progress (%) */
		} else {
			gtk_list_store_clear(ripper_prog_store);
		}

		ripper_prog_store_make(device_path);
		ripper_window();

		ripper_thread_busy = 1;
                AQUALUNG_THREAD_CREATE(ripper_thread_id, NULL, ripper_thread, (void *)strdup(device_path))
	}
}


#else

GtkWidget * ripper_prog_window = NULL;

#endif /* HAVE_CDDA && ... */

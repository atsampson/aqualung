/*                                                     -*- linux-c -*-
    Copyright (C) 2007 Tom Szilagyi

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

#ifndef _UTILS_H
#define _UTILS_H

#include <libxml/xmlmemory.h>
#include <libxml/parser.h>

#include <config.h>
#include "common.h"


/* Please update when we reach the 22nd century. */
#define YEAR_MIN 1900
#define YEAR_MAX 2100


float convf(char * s);
int cut_trailing_whitespace(char * str);
void escape_percents(char * in, char * out);

int make_string_va(char * buf, char * format, ...);
void make_title_string(char * dest, char * templ,
		       char * artist, char * record, char * track);
void make_string_strerror(int ret, char * buf);

void sample2time(unsigned long SR, unsigned long long sample, char * str, int sign);
void time2time(float samples, char * str);
void time2time_na(float seconds, char * str);

void normalize_filename(const char * in, char * out);

void free_strdup(char ** s, const char * str);
int is_valid_year(int y);
int is_all_wspace(char * str);

int cdda_is_cdtrack(char * file);

int is_dir(char * path);

#ifndef HAVE_STRNDUP
char * strndup(char * str, size_t len);
#endif /* HAVE_STRNDUP */

#ifndef HAVE_STRCASESTR
char * strcasestr(char * haystack, char * needle);
#endif /* HAVE_STRCASESTR */


typedef struct _map_t {

	char str[MAXLEN];
	int count;
	struct _map_t * next;

} map_t;

void map_put(map_t ** map, char * str);
char * map_get_max(map_t * map);
void map_free(map_t * map);

void xml_save_str(xmlNodePtr node, char * varname, char * var);
void xml_save_int(xmlNodePtr node, char * varname, int var);
void xml_save_uint(xmlNodePtr node, char * varname, unsigned var);
void xml_save_float(xmlNodePtr node, char * varname, float var);
void xml_save_int_array(xmlNodePtr node, char * varname, int * var, int idx);

void xml_load_str(xmlDocPtr doc, xmlNodePtr node, char * varname, char * var);
void xml_load_str_dup(xmlDocPtr doc, xmlNodePtr node, char * varname, char ** var);
void xml_load_int(xmlDocPtr doc, xmlNodePtr node, char * varname, int * var);
void xml_load_uint(xmlDocPtr doc, xmlNodePtr node, char * varname, unsigned * var);
void xml_load_float(xmlDocPtr doc, xmlNodePtr node, char * varname, float * var);
void xml_load_int_array(xmlDocPtr doc, xmlNodePtr node, char * varname, int * var, int idx);


#endif /* _UTILS_H */

// vim: shiftwidth=8:tabstop=8:softtabstop=8:


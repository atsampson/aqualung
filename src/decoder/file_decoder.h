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

#ifndef AQUALUNG_FILE_DECODER_H
#define AQUALUNG_FILE_DECODER_H

#include <math.h>

#include "../common.h"
#include "../metadata.h"


#ifdef __cplusplus
extern "C" {
#endif

/* input libs */
#define NULL_LIB    0
#define CDDA_LIB    1
#define SNDFILE_LIB 2
#define FLAC_LIB    3
#define VORBIS_LIB  4
#define SPEEX_LIB   5
#define MPC_LIB     6
#define MAD_LIB     7
#define MOD_LIB     8
#define MAC_LIB     9
#define LAVC_LIB    10
#define WAVPACK_LIB 11


/* format_flags */
#define FORMAT_VBR 0x0001
#define FORMAT_UBR 0x0002

typedef struct _fileinfo_t {
        unsigned long long total_samples;
        unsigned long sample_rate;
	int channels;
        int is_mono;
        int bps;
	char * format_str; /* buffer allocated in pdec */
	int format_flags;
} fileinfo_t;


typedef struct _file_decoder_t {

	/* public */
	char * filename;
	int file_open;
	int file_lib;
	fileinfo_t fileinfo;
	unsigned long long sample_pos; /* used w/unknown length files only */
	unsigned long long samples_left;
	float voladj_db;
	float voladj_lin;
	int is_stream;

	/* Note that the metadata block sent by meta_cb is still owned by
	   the file_decoder instance and should not be freed externally.
	   However, it can be modified and 'if (meta->writable)', it can be
	   written back to file by calling meta_write().
	*/
	metadata_t * meta;
	void (* meta_cb)(metadata_t *, void *);
	void * meta_cbdata;
	/* meta_write() returns one of META_ERROR_*, defined in metadata_api.h */
	int (* meta_write)(struct _file_decoder_t *, metadata_t *);

	/* private */
	void * pdec; /* actually, it's (decoder_t *) */

} file_decoder_t;


typedef struct _decoder_t {

	file_decoder_t * fdec;
	void * pdata; /* opaque pointer to decoder-dependent struct */

	struct _decoder_t * (* init)(file_decoder_t * fdec);
	void (* destroy)(struct _decoder_t * dec);
	int (* open)(struct _decoder_t * dec, char * filename);
	void (* send_metadata)(struct _decoder_t * dec);
	void (* set_rva)(struct _decoder_t * dec, float voladj);
	void (* close)(struct _decoder_t * dec);
	unsigned int (* read)(struct _decoder_t * dec, float * dest, int num);
	void (* seek)(struct _decoder_t * dec, unsigned long long seek_to_pos);

	/* optional callbacks for stream decoders */
	void (* pause)(struct _decoder_t * dec);
	void (* resume)(struct _decoder_t * dec);

	char format_str[MAXLEN];
	int format_flags;
} decoder_t;


/* return values from decoder_t.open() -- see dec_null.c for explanation */
#define DECODER_OPEN_SUCCESS 0
#define DECODER_OPEN_BADLIB  1
#define DECODER_OPEN_FERROR  2


int is_valid_extension(char ** valid_extensions, char * filename, int module);

void file_decoder_init(void);

file_decoder_t * file_decoder_new(void);
void file_decoder_delete(file_decoder_t * fdec);

int file_decoder_open(file_decoder_t * fdec, char * filename);
void file_decoder_send_metadata(file_decoder_t * fdec);
void file_decoder_set_rva(file_decoder_t * fdec, float voladj);
void file_decoder_set_meta_cb(file_decoder_t * fdec,
			      void (* meta_cb)(metadata_t * meta, void * data),
			      void * data);
void file_decoder_close(file_decoder_t * fdec);
unsigned int file_decoder_read(file_decoder_t * fdec, float * dest, int num);
void file_decoder_seek(file_decoder_t * fdec, unsigned long long seek_to_pos);

void file_decoder_pause(file_decoder_t * fdec);
void file_decoder_resume(file_decoder_t * fdec);
    
float get_file_duration(char * file);

int bigendianp(void);

#define db2lin(x) ((x) > -90.0f ? powf(10.0f, (x) * 0.05f) : 0.0f)


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AQUALUNG_FILE_DECODER_H */

// vim: shiftwidth=8:tabstop=8:softtabstop=8 :  


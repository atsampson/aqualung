/*                                                     -*- linux-c -*-
    Copyright (C) 2005 Tom Szilagyi

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

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <glib.h>

#include "../athread.h"
#include "../httpc.h"
#include "../i18n.h"
#include "../metadata.h"
#include "../metadata_api.h"
#include "../metadata_ape.h"
#include "../metadata_id3v1.h"
#include "../metadata_id3v2.h"
#include "../rb.h"
#include "file_decoder.h"
#include "dec_mpeg.h"


extern size_t sample_size;


/* Uncomment this to get debug printouts */
/* #define MPEG_DEBUG */

#define BYTES2INT(b1,b2,b3,b4) (((long)(b1 & 0xFF) << (3*8)) | \
                                ((long)(b2 & 0xFF) << (2*8)) | \
                                ((long)(b3 & 0xFF) << (1*8)) | \
                                ((long)(b4 & 0xFF) << (0*8)))

#define SYNC_MASK (0x7ffL << 21)
#define VERSION_MASK (3L << 19)
#define LAYER_MASK (3L << 17)
#define PROTECTION_MASK (1L << 16)
#define BITRATE_MASK (0xfL << 12)
#define SAMPLERATE_MASK (3L << 10)
#define PADDING_MASK (1L << 9)
#define PRIVATE_MASK (1L << 8)
#define CHANNELMODE_MASK (3L << 6)
#define MODE_EXT_MASK (3L << 4)
#define COPYRIGHT_MASK (1L << 3)
#define ORIGINAL_MASK (1L << 2)
#define EMPHASIS_MASK 3L


/* list of accepted file extensions */
char * valid_extensions_mpeg[] = {
	"mp3", "mpa", "mpga", "mpega", "abs", "mp2", "mp2a", "mpa2", "mp1", NULL
};


/* MPEG Version table, sorted by version index */
static const signed char version_table[4] = {
	MPEG_VERSION2_5, -1, MPEG_VERSION2, MPEG_VERSION1
};

/* Bitrate table for mpeg audio, indexed by row index and birate index */
static const short bitrates[5][16] = {
	{0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0}, /* V1 L1 */
	{0,32,48,56, 64, 80, 96,112,128,160,192,224,256,320,384,0}, /* V1 L2 */
	{0,32,40,48, 56, 64, 80, 96,112,128,160,192,224,256,320,0}, /* V1 L3 */
	{0,32,48,56, 64, 80, 96,112,128,144,160,176,192,224,256,0}, /* V2 L1 */
	{0, 8,16,24, 32, 40, 48, 56, 64, 80, 96,112,128,144,160,0}  /* V2 L2+L3 */
};

/* Bitrate pointer table, indexed by version and layer */
static const short *bitrate_table[3][3] = {
	{bitrates[0], bitrates[1], bitrates[2]},
	{bitrates[3], bitrates[4], bitrates[4]},
	{bitrates[3], bitrates[4], bitrates[4]}
};

/* Sampling frequency table, indexed by version and frequency index */
static const long freq_table[3][3] = {
	{44100, 48000, 32000}, /* MPEG Version 1 */
	{22050, 24000, 16000}, /* MPEG version 2 */
	{11025, 12000,  8000}, /* MPEG version 2.5 */
};

/* check if 'head' is a valid mp3 frame header */
static int
is_mp3frameheader(unsigned long head, int is_ubr_allowed) {

	if ((head & SYNC_MASK) != (unsigned long)SYNC_MASK) /* bad sync? */
		return 0;
	if ((head & VERSION_MASK) == (1L << 19)) /* bad version? */
		return 0;
	if (!(head & LAYER_MASK)) /* no layer? */
		return 0;
	if ((head & BITRATE_MASK) == BITRATE_MASK) /* bad bitrate? */
		return 0;
	if ((head & BITRATE_MASK) == 0 && !is_ubr_allowed) /* no bitrate, no UBR */
		return 0;
	if ((head & SAMPLERATE_MASK) == SAMPLERATE_MASK) /* bad sample rate? */
		return 0;

	return 1;
}

static int
mp3headerinfo(mp3info_t *info, unsigned long header) {

	int bitindex, freqindex;

	/* MPEG Audio Version */
	info->version = version_table[(header & VERSION_MASK) >> 19];
	if (info->version < 0)
		return 0;

	/* Layer */
	info->layer = 3 - ((header & LAYER_MASK) >> 17);
	if (info->layer == 3)
		return 0;

	info->protection = (header & PROTECTION_MASK) ? 1 : 0;

	/* Bitrate */
	bitindex = (header & BITRATE_MASK) >> 12;
	info->bitrate = bitrate_table[info->version][info->layer][bitindex];

	/* Sampling frequency */
	freqindex = (header & SAMPLERATE_MASK) >> 10;
	if (freqindex == 3)
		return 0;
	info->frequency = freq_table[info->version][freqindex];

	info->padding = (header & PADDING_MASK) ? 1 : 0;

	/* Calculate number of bytes, calculation depends on layer */
	if (info->layer == 0) {
		info->frame_samples = 384;
		if (info->bitrate == 0) {
			info->frame_size = 0;
		} else {
			info->frame_size = (12000 * info->bitrate / info->frequency
					    + info->padding) * 4;
		}
	} else {
		if ((info->version > MPEG_VERSION1) && (info->layer == 2)) {
			info->frame_samples = 576;
		} else {
			info->frame_samples = 1152;
		}
		if (info->bitrate == 0) {
			info->frame_size = 0;
		} else {
			info->frame_size = (1000/8) * info->frame_samples * info->bitrate
				/ info->frequency + info->padding;
		}
	}

	/* Frametime fraction calculation.
	   This fraction is reduced as far as possible. */
	if (freqindex != 0) { /* 48/32/24/16/12/8 kHz */
		/* integer number of milliseconds, denominator == 1 */
		info->ft_num = 1000 * info->frame_samples / info->frequency;
		info->ft_den = 1;
	} else {                /* 44.1/22.05/11.025 kHz */
		if (info->layer == 0) {
			info->ft_num = 147000 * 384 / info->frequency;
			info->ft_den = 147;
		}
		else {
			info->ft_num = 49000 * info->frame_samples / info->frequency;
			info->ft_den = 49;
		}
	}

	info->channel_mode = (header & CHANNELMODE_MASK) >> 6;
	info->mode_extension = (header & MODE_EXT_MASK) >> 4;
	info->emphasis = header & EMPHASIS_MASK;

#ifdef MPEG_DEBUG
/*
	printf( "Header: %08x, Ver %d, lay %d, bitr %d, freq %ld, "
		"chmode %d, mode_ext %d, emph %d, bytes: %d time: %d/%d\n",
		header, info->version, info->layer+1, info->bitrate,
		info->frequency, info->channel_mode, info->mode_extension,
		info->emphasis, info->frame_size, info->ft_num, info->ft_den);
*/
#endif /* MPEG_DEBUG */
	return 1;
}

static unsigned long
__find_next_frame(int fd, long *offset, long max_offset,
		  unsigned long last_header,
		  int(*getfunc)(int fd, unsigned char *c),
		  int is_ubr_allowed) {

	unsigned long header=0;
	unsigned char tmp;
	int i;

	long pos = 0;

	/* We remember the last header we found, to use as a template to see if
	   the header we find has the same frequency, layer etc */
	last_header &= 0xffff0c00;

	/* Fill up header with first 24 bits */
	for(i = 0; i < 3; i++) {
		header <<= 8;
		if(!getfunc(fd, &tmp))
			return 0;
		header |= tmp;
		pos++;
	}

	do {
		header <<= 8;
		if(!getfunc(fd, &tmp))
			return 0;
		header |= tmp;
		pos++;
		if (max_offset > 0 && pos > max_offset)
			return 0;
	} while(!is_mp3frameheader(header, is_ubr_allowed) ||
		(last_header?((header & 0xffff0c00) != last_header):0));

	*offset = pos - 4;

#ifdef MPEG_DEBUG
/*	if(*offset)
		printf("Warning: skipping %d bytes of garbage\n", *offset);
*/
#endif /* MPEG_DEBUG */

	return header;
}

static int
fileread(int fd, unsigned char *c) {
	return read(fd, c, 1);
}

unsigned long
find_next_frame(int fd, long *offset, long max_offset,
		unsigned long last_header, int is_ubr_allowed) {
	return __find_next_frame(fd, offset, max_offset,
				 last_header, fileread, is_ubr_allowed);
}


/* compare two headers according to version, layer, frequency, channel_mode */
int
vlfc_cmp(mp3info_t *info1, mp3info_t *info2) {

	if (info1->version != info2->version)
		return 0;
	if (info1->layer != info2->layer)
		return 0;
	if (info1->frequency != info2->frequency)
		return 0;
	if (info1->channel_mode != info2->channel_mode)
		return 0;
	return 1;
}


int
mpeg_write_metadata(file_decoder_t * fdec, metadata_t * meta) {

	int ret;

	/* write ID3v1 */
	if (metadata_get_frame_by_tag(meta, META_TAG_ID3v1, NULL) != NULL) {
		unsigned char id3v1[128];
		ret = metadata_to_id3v1(meta, id3v1);
		if (ret != META_ERROR_NONE) {
			return ret;
		}
		ret = meta_id3v1_rewrite(fdec->filename, id3v1);
		if (ret != META_ERROR_NONE) {
			return ret;
		}
	} else {
		ret = meta_id3v1_delete(fdec->filename);
		if (ret != META_ERROR_NONE) {
			return ret;
		}
	}

	/* write ID3v2 */
	if (metadata_get_frame_by_tag(meta, META_TAG_ID3v2, NULL) != NULL) {
		unsigned char * buf;
		int length;
		int ret = metadata_to_id3v2(meta, &buf, &length);
		if (ret != META_ERROR_NONE) {
			return ret;
		}
		ret = meta_id3v2_rewrite(fdec->filename, &buf, &length);
		if (ret != META_ERROR_NONE) {
			free(buf);
			return ret;
		}
		free(buf);
	} else {
		ret = meta_id3v2_delete(fdec->filename);
		if (ret != META_ERROR_NONE) {
			return ret;
		}
	}

	/* write APE */
	ret = meta_ape_write_metadata(fdec, meta);
	if (ret != META_ERROR_NONE) {
		return ret;
	}

	return META_ERROR_NONE;
}


void
mpeg_send_metadata(file_decoder_t * fdec, int fd) {

	metadata_t * meta;

	unsigned char id3v1[128];
	unsigned char * id3v2;
	unsigned long file_size;

	unsigned char buffer[12];
	guint32 id3v2_length = 0;

	ape_tag_t tag;

	meta = metadata_new();
	if (meta == NULL) {
		return;
	}

	/* read ID3v1 */
	file_size = lseek(fd, -128, SEEK_END) + 128;
	if (read(fd, id3v1, 128) == 128) {
		metadata_from_id3v1(meta, id3v1);
	}

	/* read ID3v2 */
	lseek(fd, 0L, SEEK_SET);
	if (read(fd, buffer, 10) == 10) {
		if ((buffer[0] != 'I') || (buffer[1] != 'D') || (buffer[2] != '3')) {
			lseek(fd, 0L, SEEK_SET);
		} else {
			id3v2_length = meta_id3v2_read_synchsafe_int(buffer+6);
			id3v2_length += 10; /* add 10 byte header */

#ifdef MPEG_DEBUG
			printf("id3v2_length = %ld\n", id3v2_length);
#endif /* MPEG_DEBUG */

			if (id3v2_length > file_size) {
				fprintf(stderr, "error: ID3v2 tag length greater than file length\n");
				return;
			}

			id3v2 = malloc(id3v2_length);
			if (id3v2 == NULL) {
				fprintf(stderr, "mpeg_send_metadata: malloc error\n");
				return;
			}
			lseek(fd, 0L, SEEK_SET);
			if (read(fd, id3v2, id3v2_length) != id3v2_length) {
				fprintf(stderr, "mpeg_send_metadata: error reading ID3v2 tag\n");
				return;
			}

			metadata_from_id3v2(meta, id3v2, id3v2_length);
			free(id3v2);

			lseek(fd, id3v2_length, SEEK_SET);
		}
	}

	/* read APE */
	memset(&tag, 0x00, sizeof(ape_tag_t));
	if (meta_ape_parse(fdec->filename, &tag)) {
		metadata_from_ape_tag(meta, &tag);
		meta_ape_free(&tag);
	}

	/* setup and send metablock */
	meta->valid_tags = META_TAG_APE | META_TAG_ID3v1 | META_TAG_ID3v2;

	if (access(fdec->filename, R_OK | W_OK) == 0) {
		meta->writable = 1;
		fdec->meta_write = mpeg_write_metadata;
	} else {
		meta->writable = 0;
	}

	meta->fdec = fdec;
	fdec->meta = meta;
}


int
get_mp3file_info(decoder_t * dec) {

	file_decoder_t * fdec = dec->fdec;
	mpeg_pdata_t * pd = (mpeg_pdata_t *)dec->pdata;
	int fd = pd->fd;
	mp3info_t * info = &pd->mp3info;

	long offset;
	unsigned long header = 0;
	unsigned char frame[1800];
	unsigned char *vbrheader;
	int is_ubr_allowed = 1;
	long bytecount = 0;
	int num_offsets;
	long tmp;
	int ret;

 begin_info:
	/* skip ID3v2 header, if present */
	mpeg_send_metadata(fdec, fd);

	/* use first valid mpeg frame header to retrieve stream info */
	header = find_next_frame(fd, &bytecount, 0x100000, 0, is_ubr_allowed);
	/* Quit if we haven't found a valid header within 1M */
	if (header == 0)
		return -1;

	memset(info, 0, sizeof(mp3info_t));

	/* These two are needed for proper LAME gapless MP3 playback */
	info->enc_delay = -1;
	info->enc_padding = -1;
	if (!mp3headerinfo(info, header))
		return -2;

	offset = lseek(fd, 0, SEEK_CUR);

	lseek(fd, offset, SEEK_SET);
	memset(info, 0, sizeof(mp3info_t));
	info->start_byteoffset = offset - 4;
	info->enc_delay = -1;
	info->enc_padding = -1;
	if (!mp3headerinfo(info, header))
		return -2;

#ifdef MPEG_DEBUG
	printf("start_byteoffset = %d\n", info->start_byteoffset);
#endif

	if (info->frame_size == 0) {
		/* free-format MPEG - no VBR allowed.
		   we have to find the next frame, so we can determine the frame size & bitrate */

		long offset_next;
		long header_next;
		mp3info_t info_next;
		int cntdown = 8;

		do {
			header_next = find_next_frame(fd, &bytecount, 0x20000, 0, 1);
			/* Quit if we haven't found a valid header within 128K */
			if (header_next == 0)
				return -1;

			memset(&info_next, 0, sizeof(mp3info_t));

			/* These two are needed for proper LAME gapless MP3 playback */
			info_next.enc_delay = -1;
			info_next.enc_padding = -1;
			if (!mp3headerinfo(&info_next, header_next))
				return -2;

			--cntdown;

		} while (cntdown > 0 && !vlfc_cmp(info, &info_next));

		if (cntdown == 0 && is_ubr_allowed) {
			fprintf(stderr, "dec_mpeg: corrupt stream, falling back to non-UBR only detection.\n");
			is_ubr_allowed = 0;
			lseek(fd, 0, SEEK_SET);
			goto begin_info;
		}

		offset_next = lseek(fd, 0, SEEK_CUR);
		info->frame_size = offset_next - offset;
		lseek(fd, offset - 4, SEEK_SET); /* go back to beginning of stream */
		return bytecount;
	}


	/* OK, we have the first real frame. Let's see if it has a Xing header */
	if ((ret = read(fd, frame, info->frame_size-4)) < 0) {
#ifdef MPEG_DEBUG
		printf("read(): %s\n", strerror(ret));
#endif /* MPEG_DEBUG */
		return -3;
	}

	/* calculate position of VBR header */
	if (info->version == MPEG_VERSION1) {
		if (info->channel_mode == 3) /* mono */
			vbrheader = frame + 17;
		else
			vbrheader = frame + 32;
	} else {
		if (info->channel_mode == 3) /* mono */
			vbrheader = frame + 9;
		else
			vbrheader = frame + 17;
	}

	if (!memcmp(vbrheader, "Xing", 4) || !memcmp(vbrheader, "Info", 4)) {

		int i = 8; /* Where to start parsing info */

#ifdef MPEG_DEBUG
		printf("Xing/Info header\n");
#endif /* MPEG_DEBUG */

		/* Remember where in the file the Xing header is */
		info->vbr_header_pos = lseek(fd, 0, SEEK_CUR) - info->frame_size;

		/* We want to skip the Xing frame when playing the stream */
		bytecount += info->frame_size;

		/* workaround some files that have padding bit set, but frame size is 417 bytes */
		if (info->padding) {
			lseek(fd, -1, SEEK_CUR);
		}

		/* Now get the next frame to find out the real info about
		   the mp3 stream */
		header = find_next_frame(fd, &tmp, 0x20000, 0, is_ubr_allowed);
		if(header == 0)
			return -4;

		if(!mp3headerinfo(info, header))
			return -5;

		/* Is it a VBR file? */
		info->is_vbr = info->is_xing_vbr = !memcmp(vbrheader, "Xing", 4);

		if (vbrheader[7] & VBR_FRAMES_FLAG) { /* Is the frame count there? */

			info->frame_count = BYTES2INT(vbrheader[i], vbrheader[i+1],
						      vbrheader[i+2], vbrheader[i+3]);
			if (info->frame_count <= ULONG_MAX / info->ft_num)
				info->file_time = info->frame_count * info->ft_num / info->ft_den;
			else
				info->file_time = info->frame_count / info->ft_den * info->ft_num;
			i += 4;
		}

		if (vbrheader[7] & VBR_BYTES_FLAG) { /* Is byte count there? */
			info->byte_count = BYTES2INT(vbrheader[i], vbrheader[i+1],
						     vbrheader[i+2], vbrheader[i+3]);
			i += 4;
		}

		if (info->file_time && info->byte_count) {
			if (info->byte_count <= (ULONG_MAX/8))
				info->bitrate = info->byte_count * 8 / info->file_time;
			else
				info->bitrate = info->byte_count / (info->file_time >> 3);
		} else {
			info->bitrate = 0;
		}

		if (vbrheader[7] & VBR_TOC_FLAG) { /* Is table-of-contents there? */
			memcpy(info->toc, vbrheader+i, 100);
			i += 100;
		}
		if (vbrheader[7] & VBR_QUALITY_FLAG) {
			/* We don't care about this, but need to skip it */
			i += 4;
		}
		i += 21;
		info->enc_delay = (vbrheader[i] << 4) | (vbrheader[i + 1] >> 4);
		info->enc_padding = ((vbrheader[i + 1] & 0x0f) << 8) | vbrheader[i + 2];
		if (!(info->enc_delay >= 0 && info->enc_delay <= 1152 &&
		      info->enc_padding >= 0 && info->enc_padding <= 2*1152)) {
			/* Invalid data */
			info->enc_delay = -1;
			info->enc_padding = -1;
		}
	}

	if (!memcmp(vbrheader, "VBRI", 4)) {
#ifdef MPEG_DEBUG
		printf("VBRI header\n");
#endif /* MPEG_DEBUG */
		int i, j;

		/* We want to skip the VBRI frame when playing the stream */
		bytecount += info->frame_size;

		/* Now get the next frame to find out the real info about
		   the mp3 stream */
		header = find_next_frame(fd, &tmp, 0x20000, 0, is_ubr_allowed);
		if(header == 0)
			return -6;

		bytecount += tmp;

		if(!mp3headerinfo(info, header))
			return -7;

#ifdef MPEG_DEBUG
		printf("%04x: %04x %04x ", 0, header >> 16, header & 0xffff);
		for(i = 4;i < (int)sizeof(frame)-4;i+=2) {
			if(i % 16 == 0) {
				printf("\n%04x: ", i-4);
			}
			printf("%04x ", (frame[i-4] << 8) | frame[i-4+1]);
		}
		printf("\n");
#endif /* MPEG_DEBUG */

		/* Yes, it is a FhG VBR file */
		info->is_vbr = 1;
		info->is_vbri_vbr = 1;
		info->has_toc = 0; /* We don't parse the TOC (yet) */

		info->byte_count = BYTES2INT(vbrheader[10], vbrheader[11],
					     vbrheader[12], vbrheader[13]);
		info->frame_count = BYTES2INT(vbrheader[14], vbrheader[15],
					      vbrheader[16], vbrheader[17]);
		if (info->frame_count <= ULONG_MAX / info->ft_num)
			info->file_time = info->frame_count * info->ft_num / info->ft_den;
		else
			info->file_time = info->frame_count / info->ft_den * info->ft_num;

		if (info->byte_count <= (ULONG_MAX/8))
			info->bitrate = info->byte_count * 8 / info->file_time;
		else
			info->bitrate = info->byte_count / (info->file_time >> 3);

		/* We don't parse the TOC, since we don't yet know how to (FIXME) */
		num_offsets = BYTES2INT(0, 0, vbrheader[18], vbrheader[19]);
#ifdef MPEG_DEBUG
		printf("Frame size (%dkpbs): %d bytes (0x%x)\n",
		       info->bitrate, info->frame_size, info->frame_size);
		printf("Frame count: %x\n", info->frame_count);
		printf("Byte count: %x\n", info->byte_count);
		printf("Offsets: %d\n", num_offsets);
		printf("Frames/entry: %d\n",
		       BYTES2INT(0, 0, vbrheader[24], vbrheader[25]));
#endif /* MPEG_DEBUG */

		offset = 0;
		for(i = 0;i < num_offsets;i++) {
			j = BYTES2INT(0, 0, vbrheader[26+i*2], vbrheader[27+i*2]);
			offset += j;
#ifdef MPEG_DEBUG
			printf("%03d: %x (%x)\n", i, offset - bytecount, j);
#endif /* MPEG_DEBUG */
		}
	}
	return bytecount;
}


void *
build_seek_table_thread(void * args) {

	mpeg_pdata_t * pd = (mpeg_pdata_t *) args;
	int cnt = 0;
	int table_index = 0;
	long long sample_offset = 0;
	char * bytes = (char *)pd->fdm;
	long i;
	int div;
	long last = 0, last_but_1 = 0;
	long limit = pd->filesize-4;

	pd->builder_thread_running = 1;

	if (pd->mp3info.is_vbr) {
		div = pd->mp3info.frame_count / 100;
	} else {
		unsigned long frame_count = (pd->filesize - pd->mp3info.start_byteoffset)
			/ pd->mp3info.frame_size;
		div = frame_count / 100;
	}

	if (div < 1) {
#ifdef MPEG_DEBUG
		printf("track is too short, not building seek table\n");
#endif /* MPEG_DEBUG */
		AQUALUNG_THREAD_DETACH()
		return NULL;
	}

#ifdef MPEG_DEBUG
	printf("building seek table, div = %d\n", div);
#endif /* MPEG_DEBUG */


	/* look for id3v1 tag at the end of file */
	if (pd->filesize >= 128 + 4) {
		if ((bytes[pd->filesize-128] == 'T') &&
		    (bytes[pd->filesize-127] == 'A') &&
		    (bytes[pd->filesize-126] == 'G')) {
#ifdef MPEG_DEBUG
			printf("ID3v1 tag found at end of file\n");
#endif /* MPEG_DEBUG */
			limit -= 128;
		}
	}


	for (i = pd->mp3info.start_byteoffset; i < limit;) {
		long header = BYTES2INT(bytes[i], bytes[i+1], bytes[i+2], bytes[i+3]);
		mp3info_t mp3info;

		if (is_mp3frameheader(header, 1)) {
			mp3headerinfo(&mp3info, header);
			if ((mp3info.layer == pd->mp3info.layer) &&
			    (mp3info.version == pd->mp3info.version) &&
			    (mp3info.channel_mode == pd->mp3info.channel_mode) &&
			    (mp3info.frequency == pd->mp3info.frequency)) {

				last_but_1 = last;
				last = i;

				if ((cnt % div == 0) && (table_index < 100)) {
#ifdef MPEG_DEBUG
					printf("idx %2d | offset %8ld | sample %9lld\n",
					       table_index, i, sample_offset);
#endif /* MPEG_DEBUG */
					pd->seek_table[table_index].frame = cnt;
					pd->seek_table[table_index].sample = sample_offset;
					pd->seek_table[table_index].offset = i;
					++table_index;
				}

				sample_offset += mp3info.frame_samples;
				if (mp3info.frame_size == 0) {
					i += pd->mp3info.frame_size;
				} else {
					i += mp3info.frame_size;
				}
				++cnt;
			} else {
				++i;
			}
		} else {
			++i;
		}

		if (!pd->builder_thread_running) {
			/* we were cancelled by the file decoder main thread */
#ifdef MPEG_DEBUG
			printf("seek table builder thread cancelled, exiting.\n");
#endif /* MPEG_DEBUG */
			AQUALUNG_THREAD_DETACH()
			return NULL;
		}
	}

	if (table_index > 0 && table_index < 100) {
		int j;
		for (j = table_index; j < 100; j++) {
			pd->seek_table[j].frame = pd->seek_table[table_index-1].frame;
			pd->seek_table[j].sample = pd->seek_table[table_index-1].sample;
			pd->seek_table[j].offset = pd->seek_table[table_index-1].offset;
		}
	}

#ifdef MPEG_DEBUG
	{
		i = last;
		long header = BYTES2INT(bytes[i], bytes[i+1], bytes[i+2], bytes[i+3]);
		mp3info_t mp3info;
		mp3headerinfo(&mp3info, header);
		printf("last frame position = %ld\n", limit + 4 - i);
		printf("last frame length = %d samp = %d\n",
		       mp3info.frame_size, mp3info.frame_samples);
	}
#endif /* MPEG_DEBUG */

	pd->last_frames[1] = last_but_1;
	pd->last_frames[0] = last;

#ifdef MPEG_DEBUG
	printf("seek table builder thread finished, cnt = %d\n", cnt);
	printf("last_frames[1] = %ld\n", pd->last_frames[1]);
	printf("last_frames[0] = %ld\n", pd->last_frames[0]);
#endif /* MPEG_DEBUG */
	pd->builder_thread_running = 0;
	AQUALUNG_THREAD_DETACH()
	return NULL;
}


void
build_seek_table(mpeg_pdata_t * pd) {

	AQUALUNG_THREAD_CREATE(pd->seek_builder_id, NULL, build_seek_table_thread, pd)
}



void
pause_mpeg_stream(decoder_t * dec) {

	mpeg_pdata_t * pd = (mpeg_pdata_t *)dec->pdata;
	char flush_dest;

	httpc_close(pd->session);

	if (pd->session->type == HTTPC_SESSION_STREAM) {
		/* empty mpeg decoder ringbuffer */
		while (rb_read_space(pd->rb))
			rb_read(pd->rb, &flush_dest, sizeof(char));
	}
}


void
resume_mpeg_stream(decoder_t * dec) {

	mpeg_pdata_t * pd = (mpeg_pdata_t *)dec->pdata;
	httpc_reconnect(pd->session);
}

/* MPEG input callback */
static
enum mad_flow
mpeg_input_stream(decoder_t * dec, struct mad_stream * stream) {

	mpeg_pdata_t * pd = (mpeg_pdata_t *)dec->pdata;
	size_t remaining;
	size_t read_length;
	unsigned char * read_start;
	unsigned char * guard_ptr;

	if (stream->next_frame != NULL) {
		remaining = stream->bufend - stream->next_frame;
		memmove(pd->inbuf, stream->next_frame, remaining);
		read_start = pd->inbuf + remaining;
		read_length = MPEG_INBUF_SIZE - remaining;
	} else {
		read_length = MPEG_INBUF_SIZE;
		read_start = pd->inbuf;
		remaining = 0;
	}

	read_length = httpc_read(pd->session, (char *)read_start, read_length);
	if (read_length < 0)
		return MAD_FLOW_STOP;

	if (read_length == 0) {
		guard_ptr = read_start + read_length;
		memset(guard_ptr, 0, MAD_BUFFER_GUARD);
		read_length += MAD_BUFFER_GUARD;
	}

	mad_stream_buffer(stream, pd->inbuf, read_length + remaining);
	stream->error=0;

        return MAD_FLOW_CONTINUE;
}


static
enum mad_flow
mpeg_input(void * data, struct mad_stream * stream) {

        decoder_t * dec = (decoder_t *)data;
	file_decoder_t * fdec = dec->fdec;
	mpeg_pdata_t * pd = (mpeg_pdata_t *)dec->pdata;
	size_t size = 0;

	if (fdec->is_stream) {
		return mpeg_input_stream(dec, stream);
	}

        if (fstat(pd->fd, &(pd->mpeg_stat)) == -1 || pd->mpeg_stat.st_size == 0)
                return MAD_FLOW_STOP;

	size = pd->mpeg_stat.st_size;

        pd->fdm = mmap(0, size, PROT_READ, MAP_SHARED, pd->fd, 0);
        if (pd->fdm == MAP_FAILED)
                return MAD_FLOW_STOP;

        mad_stream_buffer(stream, pd->fdm + pd->mp3info.start_byteoffset,
			  size - pd->mp3info.start_byteoffset);

        return MAD_FLOW_CONTINUE;
}


/* MPEG output callback */
static
enum mad_flow
mpeg_output(void * data, struct mad_header const * header, struct mad_pcm * pcm) {

        decoder_t * dec = (decoder_t *)data;
	mpeg_pdata_t * pd = (mpeg_pdata_t *)dec->pdata;
	file_decoder_t * fdec = dec->fdec;
	long pos_bytes;
	int end_count = pcm->length;

	int i = 0, j;
	unsigned long scale = 322122547; /* (1 << 28) * 1.2 */
        int buf[2];
        float fbuf[2];

	int pad = pd->mp3info.enc_padding;

	if (pd->delay_frames > pcm->length) {
		pd->delay_frames -= pcm->length;
#ifdef MPEG_DEBUG
		printf("skipping whole frame as part of encoder delay\n");
#endif /* MPEG_DEBUG */
		return MAD_FLOW_CONTINUE;
	} else if (pd->delay_frames > 0) {
		i = pd->delay_frames;
		pd->delay_frames = 0;
#ifdef MPEG_DEBUG
		printf("skipping %d samples of encoder delay\n", i);
#endif /* MPEG_DEBUG */
	} else {
		i = 0;
	}

	pos_bytes = (pd->mpeg_stream.this_frame - pd->mpeg_stream.buffer) + pd->mp3info.start_byteoffset + 10;
	if ((pad > 0) && (pd->last_frames[0] != -1) && (pos_bytes >= pd->last_frames[0])) {
#ifdef MPEG_DEBUG
		printf(" *** last frame len=%d ***\n", pcm->length);
#endif /* MPEG_DEBUG */
		if (pad > pcm->length) {
#ifdef MPEG_DEBUG
			printf("skipping whole frame\n");
#endif /* MPEG_DEBUG */
			return MAD_FLOW_CONTINUE;
		} else {
			end_count = pcm->length - pad;
#ifdef MPEG_DEBUG
			printf("skipping %d samples\n", pad);
#endif /* MPEG_DEBUG */
		}
	} else if ((pad > 0) && (pd->last_frames[1] != -1) && (pos_bytes >= pd->last_frames[1])) {
#ifdef MPEG_DEBUG
		printf(" *** last but one frame len=%d***\n", pcm->length);
#endif /* MPEG_DEBUG */
		if (pad > pcm->length) {
			pad -= pcm->length;
			end_count = pcm->length - pad;
#ifdef MPEG_DEBUG
			printf("skipping %d samples\n", pad);
#endif /* MPEG_DEBUG */
		}
	}

        for (; i < end_count; i++) {
                for (j = 0; j < pd->channels; j++) {
                        buf[j] = pd->error ? 0 : *(pcm->samples[j] + i);
                        fbuf[j] = (double)buf[j] * fdec->voladj_lin / scale;
                }
		if (fdec->is_stream && pcm->channels == 1) {
			fbuf[1] = fbuf[0];
		}
                if (rb_write_space(pd->rb) >= pd->channels * sample_size) {
                        rb_write(pd->rb, (char *)fbuf, pd->channels * sample_size);
		}
        }
	pd->frame_counter++;
        pd->error = 0;

        return MAD_FLOW_CONTINUE;
}


/* MPEG header callback */
static
enum mad_flow
mpeg_header(void * data, struct mad_header const * header) {

        decoder_t * dec = (decoder_t *)data;
	mpeg_pdata_t * pd = (mpeg_pdata_t *)dec->pdata;
	file_decoder_t * fdec = dec->fdec;

	if (pd->bitrate != header->bitrate) {
		pd->bitrate = header->bitrate;
	}

	if (fdec->fileinfo.sample_rate != header->samplerate) {
		fdec->fileinfo.sample_rate = header->samplerate;
	}

        return MAD_FLOW_CONTINUE;
}


/* MPEG error callback */
static
enum mad_flow
mpeg_error(void * data, struct mad_stream * stream, struct mad_frame * frame) {

        decoder_t * dec = (decoder_t *)data;
	mpeg_pdata_t * pd = (mpeg_pdata_t *)dec->pdata;

        pd->error = 1;

        return MAD_FLOW_CONTINUE;
}


/* Main decode loop: return 1 if reached end of stream, 0 else */
int
decode_mpeg(decoder_t * dec) {

	mpeg_pdata_t * pd = (mpeg_pdata_t *)dec->pdata;
	file_decoder_t * fdec = dec->fdec;

        if (mad_header_decode(&(pd->mpeg_frame.header), &(pd->mpeg_stream)) == -1) {
                if (pd->mpeg_stream.error == MAD_ERROR_BUFLEN) {
			if (fdec->is_stream) {
				if (mpeg_input_stream(dec, &(pd->mpeg_stream)) == MAD_FLOW_STOP)
					return 1;
			} else {
				return 1;
			}
		}


                if (pd->mpeg_stream.error != MAD_ERROR_NONE &&
		    !MAD_RECOVERABLE(pd->mpeg_stream.error)) {
                        fprintf(stderr, "libMAD: unrecoverable error in MPEG Audio stream\n");
			mpeg_error((void *)dec, &(pd->mpeg_stream), &(pd->mpeg_frame));
		} else {
			pd->error = 0;
		}
        }
        mpeg_header((void *)dec, &(pd->mpeg_frame.header));

        if (mad_frame_decode(&(pd->mpeg_frame), &(pd->mpeg_stream)) == -1) {
                if (pd->mpeg_stream.error == MAD_ERROR_BUFLEN)
                        return 1;

                if (pd->mpeg_stream.error != MAD_ERROR_NONE &&
		    !MAD_RECOVERABLE(pd->mpeg_stream.error))
                        fprintf(stderr, "libMAD: unrecoverable error in MPEG Audio stream\n");

                mpeg_error((void *)dec, &(pd->mpeg_stream), &(pd->mpeg_frame));
        }

        mad_synth_frame(&(pd->mpeg_synth), &(pd->mpeg_frame));
        mpeg_output((void *)dec, &(pd->mpeg_frame.header), &(pd->mpeg_synth.pcm));

        return 0;
}


decoder_t *
mpeg_decoder_init(file_decoder_t * fdec) {

        decoder_t * dec = NULL;

        if ((dec = calloc(1, sizeof(decoder_t))) == NULL) {
                fprintf(stderr, "dec_mpeg.c: mpeg_decoder_new() failed: calloc error\n");
                return NULL;
        }

	dec->fdec = fdec;

        if ((dec->pdata = calloc(1, sizeof(mpeg_pdata_t))) == NULL) {
                fprintf(stderr, "dec_mpeg.c: mpeg_decoder_new() failed: calloc error\n");
                return NULL;
        }

	dec->init = mpeg_decoder_init;
	dec->destroy = mpeg_decoder_destroy;
	dec->open = mpeg_decoder_open;
	dec->send_metadata = mpeg_decoder_send_metadata;
	dec->close = mpeg_decoder_close;
	dec->read = mpeg_decoder_read;
	dec->seek = mpeg_decoder_seek;

	return dec;
}


void
mpeg_decoder_destroy(decoder_t * dec) {

	mpeg_pdata_t * pd = (mpeg_pdata_t *)dec->pdata;
	file_decoder_t * fdec = dec->fdec;

	if (fdec->is_stream) {
		httpc_del(pd->session);
		fdec->is_stream = 0;
	}

	free(dec->pdata);
	free(dec);
}


int
mpeg_decoder_finish_open(decoder_t * dec) {

	mpeg_pdata_t * pd = (mpeg_pdata_t *)dec->pdata;
	file_decoder_t * fdec = dec->fdec;
	int i;

	/* data init (so when seeking we know if an entry is not yet filled in) */
	for (i = 0; i < 100; i++) {
		pd->seek_table[i].frame = -1;
	}
	pd->builder_thread_running = 0;

	for (i = 0; i < 2; i++) {
		pd->last_frames[i] = -1;
	}

	pd->error = 0;
	pd->is_eos = 0;
	pd->seek_table_built = 0;
	pd->rb = rb_create(pd->channels * sample_size * RB_MAD_SIZE);
	fdec->fileinfo.channels = pd->channels;
	fdec->fileinfo.sample_rate = pd->SR;
	fdec->file_lib = MAD_LIB;
	strcpy(dec->format_str, "MPEG Audio");

	if (pd->mpeg_subformat & 0xff7) {
		strcat(dec->format_str, " (");
		switch (pd->mpeg_subformat & MPEG_LAYER_MASK) {
		case MPEG_LAYER_I:
			strcat(dec->format_str, _("Layer I"));
			break;
		case MPEG_LAYER_II:
			strcat(dec->format_str, _("Layer II"));
			break;
		case MPEG_LAYER_III:
			strcat(dec->format_str, _("Layer III"));
			break;
		default:
			strcat(dec->format_str, _("Unrecognized"));
			break;
		}
	}

	if ((pd->mpeg_subformat & MPEG_LAYER_MASK) && (pd->mpeg_subformat & (MPEG_MODE_MASK | MPEG_EMPH_MASK)))
		strcat(dec->format_str, ", ");
	switch (pd->mpeg_subformat & MPEG_MODE_MASK) {
	case MPEG_MODE_SINGLE:
		strcat(dec->format_str, _("Single channel"));
		break;
	case MPEG_MODE_DUAL:
		strcat(dec->format_str, _("Dual channel"));
		break;
	case MPEG_MODE_JOINT:
		strcat(dec->format_str, _("Joint stereo"));
		break;
	case MPEG_MODE_STEREO:
		strcat(dec->format_str, _("Stereo"));
		break;
	}

	if ((pd->mpeg_subformat & MPEG_MODE_MASK) && (pd->mpeg_subformat & MPEG_EMPH_MASK))
		strcat(dec->format_str, ", ");
	switch (pd->mpeg_subformat & MPEG_EMPH_MASK) {
	case MPEG_EMPH_NONE:
		strcat(dec->format_str, _("Emphasis: none"));
		break;
	case MPEG_EMPH_5015:
		strcat(dec->format_str, _("Emphasis:"));
		strcat(dec->format_str, " 50/15 us");
		break;
	case MPEG_EMPH_J_17:
		strcat(dec->format_str, _("Emphasis:"));
		strcat(dec->format_str, " CCITT J.17");
		break;
	case MPEG_EMPH_RES:
		strcat(dec->format_str, _("Emphasis: reserved"));
		break;
	}
	strcat(dec->format_str, ")");

	fdec->fileinfo.total_samples = pd->total_samples_est;
	fdec->fileinfo.bps = pd->bitrate;

	/* setup playback */
	mad_stream_init(&(pd->mpeg_stream));
	mad_frame_init(&(pd->mpeg_frame));
	mad_synth_init(&(pd->mpeg_synth));

	if (fdec->is_stream) {
		mad_stream_buffer(&pd->mpeg_stream, pd->inbuf, 0);
	} else {
		if (mpeg_input((void *)dec, &(pd->mpeg_stream)) == MAD_FLOW_STOP) {
			mad_synth_finish(&(pd->mpeg_synth));
			mad_frame_finish(&(pd->mpeg_frame));
			mad_stream_finish(&(pd->mpeg_stream));
			return DECODER_OPEN_FERROR;
		}
	}

	pd->frame_counter = 0;

#ifdef MPEG_DEBUG
	printf("mpeg_decoder_open successful\n");
#endif /* MPEG_DEBUG */
	return DECODER_OPEN_SUCCESS;
}


int
mpeg_decoder_open(decoder_t * dec, char * filename) {

	file_decoder_t * fdec = dec->fdec;
	mpeg_pdata_t * pd = (mpeg_pdata_t *)dec->pdata;
	struct stat exp_stat;

	if (!is_valid_extension(valid_extensions_mpeg, filename, 0)) {
#ifdef MPEG_DEBUG
		printf("invalid extension of %s\n", filename);
#endif /* MPEG_DEBUG */
		return DECODER_OPEN_BADLIB;
	}

	fdec->is_stream = 0;

	if ((pd->fd = open(filename, O_RDONLY)) == 0) {
		fprintf(stderr, "mpeg_decoder_open: open() failed for MPEG Audio file\n");
		return DECODER_OPEN_FERROR;
	}

	fstat(pd->fd, &exp_stat);
	pd->filesize = exp_stat.st_size;
	pd->SR = pd->channels = pd->bitrate = pd->mpeg_subformat = 0;
	pd->error = 0;

	pd->skip_bytes = get_mp3file_info(dec);
	close(pd->fd);

#ifdef MPEG_DEBUG
	printf("skip_bytes = %ld\n\n", pd->skip_bytes);
#endif /* MPEG_DEBUG */
	if (pd->skip_bytes < 0) {
		return DECODER_OPEN_BADLIB;
	}

#ifdef MPEG_DEBUG
	printf("version = %d\n", pd->mp3info.version);
	printf("layer = %d\n", pd->mp3info.layer);
	printf("bitrate = %d\n", pd->mp3info.bitrate);
	printf("frequency = %d\n", pd->mp3info.frequency);
	printf("channel_mode = %d\n", pd->mp3info.channel_mode);
	printf("mode_extension = %d\n", pd->mp3info.mode_extension);
	printf("emphasis = %d\n", pd->mp3info.emphasis);
	printf("frame_size = %d\n", pd->mp3info.frame_size);
	printf("frame_samples = %d\n", pd->mp3info.frame_samples);
	printf("is_vbr = %d\n", pd->mp3info.is_vbr);
	printf("has_toc = %d\n", pd->mp3info.has_toc);
	printf("frame_count = %ld\n", pd->mp3info.frame_count);
	printf("byte_count = %ld\n", pd->mp3info.byte_count);
	printf("file_time = %ld\n", pd->mp3info.file_time);
	printf("enc_delay = %ld\n", pd->mp3info.enc_delay);
	printf("enc_padding = %ld\n", pd->mp3info.enc_padding);
#endif /* MPEG_DEBUG */

	pd->SR = pd->mp3info.frequency;
	pd->bitrate = pd->mp3info.bitrate * 1000;

        switch (pd->mp3info.layer) {
        case 0: pd->mpeg_subformat |= MPEG_LAYER_I;
                break;
        case 1: pd->mpeg_subformat |= MPEG_LAYER_II;
                break;
        case 2: pd->mpeg_subformat |= MPEG_LAYER_III;
                break;
        }

        switch (pd->mp3info.channel_mode) {
        case 0: pd->mpeg_subformat |= MPEG_MODE_STEREO;
                pd->channels = 2;
                break;
        case 1: pd->mpeg_subformat |= MPEG_MODE_JOINT;
                pd->channels = 2;
                break;
        case 2: pd->mpeg_subformat |= MPEG_MODE_DUAL;
                pd->channels = 2;
                break;
        case 3: pd->mpeg_subformat |= MPEG_MODE_SINGLE;
                pd->channels = 1;
                break;
        }

        switch (pd->mp3info.emphasis) {
        case 0: pd->mpeg_subformat |= MPEG_EMPH_NONE;
                break;
        case 1: pd->mpeg_subformat |= MPEG_EMPH_5015;
                break;
        case 2: pd->mpeg_subformat |= MPEG_EMPH_RES;
                break;
        case 3: pd->mpeg_subformat |= MPEG_EMPH_J_17;
                break;
        }

	if ((!pd->SR) || (!pd->channels)) {
		return DECODER_OPEN_BADLIB;
	}

	if ((pd->channels != 1) && (pd->channels != 2)) {
		fprintf(stderr,
			"mpeg_decoder_open: MPEG Audio file with %d channels "
			"is unsupported\n", pd->channels);
		return DECODER_OPEN_FERROR;
	}

	if (pd->mp3info.is_vbr) {
		pd->total_samples_est = (double)pd->SR * pd->mp3info.file_time / 1000.0f;
		dec->format_flags |= FORMAT_VBR;
	} else {
		if (pd->bitrate == 0) {
			pd->total_samples_est = (long long)(pd->filesize - pd->mp3info.start_byteoffset)
				* pd->mp3info.frame_samples / pd->mp3info.frame_size;

			pd->bitrate = pd->mp3info.bitrate = 8 *
				(double)(pd->filesize - pd->mp3info.start_byteoffset) /
				(pd->total_samples_est / pd->SR);
			dec->format_flags |= FORMAT_UBR;
		} else {
			pd->total_samples_est =
				(double)(pd->filesize - pd->mp3info.start_byteoffset)
				/ pd->bitrate * 8 * pd->SR;
		}
	}

	if (pd->mp3info.enc_delay > 0) {
		pd->delay_frames = pd->mp3info.enc_delay + 528 + pd->mp3info.frame_samples;
	} else {
		pd->delay_frames = 0;
	}

	pd->fd = open(filename, O_RDONLY);

	return mpeg_decoder_finish_open(dec);
}


void
mpeg_decoder_send_metadata(decoder_t * dec) {

        file_decoder_t * fdec = dec->fdec;

        if (fdec->meta != NULL && fdec->meta_cb != NULL) {
		fdec->meta_cb(fdec->meta, fdec->meta_cbdata);
	}
}

int
mpeg_stream_decoder_open(decoder_t * dec, http_session_t * session) {

	mpeg_pdata_t * pd = (mpeg_pdata_t *)dec->pdata;
	file_decoder_t * fdec = dec->fdec;

	pd->inbuf = calloc(MPEG_INBUF_SIZE+MAD_BUFFER_GUARD, 1);
	if (pd->inbuf == NULL) {
		fprintf(stderr, "mpeg_stream_decoder_open: calloc error\n");
		return DECODER_OPEN_FERROR;
	}

	pd->channels = 2;
	pd->SR = 44100;
	pd->total_samples_est = 0;
	pd->bitrate = 1000 * session->headers.icy_br;

	/* XXX: this is just so we can display something */
	pd->mpeg_subformat |= MPEG_LAYER_III;
	pd->mpeg_subformat |= MPEG_MODE_STEREO;
	pd->mpeg_subformat |= MPEG_EMPH_NONE;

	fdec->is_stream = 1;
	pd->session = session;

	dec->pause = pause_mpeg_stream;
	dec->resume = resume_mpeg_stream;

	return mpeg_decoder_finish_open(dec);
}


void
mpeg_decoder_close(decoder_t * dec) {

	mpeg_pdata_t * pd = (mpeg_pdata_t *)dec->pdata;
	file_decoder_t * fdec = dec->fdec;

	/* take care of seek table builder thread, if there is any */
	if (pd->builder_thread_running) {
		pd->builder_thread_running = 0;
#ifdef MPEG_DEBUG
		printf("joining seek table builder thread\n");
#endif /* MPEG_DEBUG */
		AQUALUNG_THREAD_JOIN(pd->seek_builder_id)
#ifdef MPEG_DEBUG
		printf("joined seek table builder thread\n");
#endif /* MPEG_DEBUG */
	}

	mad_synth_finish(&(pd->mpeg_synth));
	mad_frame_finish(&(pd->mpeg_frame));
	mad_stream_finish(&(pd->mpeg_stream));
	if (fdec->is_stream) {
		free(pd->inbuf);
		httpc_close(pd->session);
	} else {
		if (munmap(pd->fdm, pd->mpeg_stat.st_size) == -1)
			fprintf(stderr, "Error while munmap()'ing MPEG Audio file mapping\n");
		close(pd->fd);
	}
	rb_free(pd->rb);
#ifdef MPEG_DEBUG
	printf("mpeg_decoder_close successful\n");
#endif /* MPEG_DEBUG */
}


unsigned int
mpeg_decoder_read(decoder_t * dec, float * dest, int num) {

	mpeg_pdata_t * pd = (mpeg_pdata_t *)dec->pdata;
	file_decoder_t * fdec = dec->fdec;

	unsigned int numread = 0;
	unsigned int n_avail = 0;

	if (!fdec->is_stream && !pd->seek_table_built) {
#ifdef MPEG_DEBUG
		printf("first read from mpeg file\n");
#endif /* MPEG_DEBUG */
		/* read mmap'ed file and build seek table in background thread.
		   we do this upon the first read, so it doesn't start when we open
		   a mass of files, but don't read any audio (metadata retrieval, etc) */
		build_seek_table(pd);
		pd->seek_table_built = 1;
	}

	while ((rb_read_space(pd->rb) < num * pd->channels *
		sample_size) && (!pd->is_eos)) {

		pd->is_eos = decode_mpeg(dec);
	}

	n_avail = rb_read_space(pd->rb) / (pd->channels * sample_size);

	if (n_avail > num)
		n_avail = num;

	rb_read(pd->rb, (char *)dest, n_avail * pd->channels * sample_size);

	numread = n_avail;
	return numread;
}


void
mpeg_decoder_seek(decoder_t * dec, unsigned long long seek_to_pos) {

	mpeg_pdata_t * pd = (mpeg_pdata_t *)dec->pdata;
	file_decoder_t * fdec = dec->fdec;
	char * bytes = (char *)pd->fdm;
	long header;
	mp3info_t mp3info;
	char flush_dest;
	int i;
	unsigned long offset;
	unsigned long long sample;
#ifdef MPEG_DEBUG
	int cnt = 0;
#endif /* MPEG_DEBUG */

	if (seek_to_pos < pd->mp3info.frame_samples) {
		pd->frame_counter = 0;
		pd->mpeg_stream.next_frame = pd->mpeg_stream.buffer;
		fdec->samples_left = fdec->fileinfo.total_samples;
		goto flush_decoder_rb;
	}

	/* search for closest preceding entry in seek table */
	for (i = 0; i < 99; i++) {
		if (pd->seek_table[i].frame == -1) {
			/* uninitialized seek table (to the desired position, at least)
			   so we fall back on conventional bitstream seeking */
#ifdef MPEG_DEBUG
			printf("seek table not yet ready, seeking bitstream.\n");
#endif /* MPEG_DEBUG */
			pd->mpeg_stream.next_frame = pd->mpeg_stream.buffer;
			mad_stream_sync(&(pd->mpeg_stream));
			mad_stream_skip(&(pd->mpeg_stream),
					(pd->filesize - pd->mp3info.start_byteoffset)
					* (double)seek_to_pos / pd->total_samples_est);
			mad_stream_sync(&(pd->mpeg_stream));

			pd->is_eos = decode_mpeg(dec);
			/* report the real position of the decoder */
			fdec->samples_left = fdec->fileinfo.total_samples -
				(pd->mpeg_stream.next_frame - pd->mpeg_stream.buffer)
				/ pd->bitrate * 8 * pd->SR;

			goto flush_decoder_rb;
		}

		if (pd->seek_table[i+1].sample > seek_to_pos)
			break;
	}

	offset = pd->seek_table[i].offset;
	sample = pd->seek_table[i].sample;
#ifdef MPEG_DEBUG
	printf("seek table: byte_offset = %d  sample_pos = %d\n", offset, sample);
#endif /* MPEG_DEBUG */

	do {
		if (offset > pd->filesize - 4) {
			offset = pd->filesize-4;
			break;
		}
		header = BYTES2INT(bytes[offset], bytes[offset+1], bytes[offset+2], bytes[offset+3]);
		if (is_mp3frameheader(header, 1)) {
			mp3headerinfo(&mp3info, header);
			if ((mp3info.layer == pd->mp3info.layer) &&
			    (mp3info.version == pd->mp3info.version) &&
			    (mp3info.frequency == pd->mp3info.frequency)) {

				sample += mp3info.frame_samples;
				offset += mp3info.frame_size;
#ifdef MPEG_DEBUG
				cnt += mp3info.frame_size;
#endif /* MPEG_DEBUG */
			} else {
				++offset;
#ifdef MPEG_DEBUG
				++cnt;
#endif /* MPEG_DEBUG */
			}
		} else {
			++offset;
#ifdef MPEG_DEBUG
			++cnt;
#endif /* MPEG_DEBUG */
		}
	} while (sample + mp3info.frame_samples < seek_to_pos);
#ifdef MPEG_DEBUG
	printf("stream nudged by %d bytes\n", cnt);
#endif /* MPEG_DEBUG */

	pd->mpeg_stream.next_frame = pd->mpeg_stream.buffer - pd->mp3info.start_byteoffset + offset;
	fdec->samples_left = fdec->fileinfo.total_samples - sample;

 flush_decoder_rb:
	/* empty mpeg decoder ringbuffer */
	while (rb_read_space(pd->rb))
		rb_read(pd->rb, &flush_dest, sizeof(char));
}



// vim: shiftwidth=8:tabstop=8:softtabstop=8 :

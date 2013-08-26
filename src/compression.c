/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 4; tab-width: 4 -*- */
/*
 * viking -- GPS Data and Topo Analyzer, Explorer, and Manager
 *
 * Copyright (C) 2003-2008, Evan Battaglia <gtoevan@gmx.net>
 * Copyright (C) 2007, Quy Tonthat <qtonthat@gmail.com>
 * Copyright (C) 2013, Rob Norris <rw_norris@hotmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_LIBZ
#include <zlib.h>
#endif

#ifdef HAVE_BZLIB_H
#include <bzlib.h>
#endif

#include "compression.h"
#include <gio/gio.h>
#include <glib/gstdio.h>

#ifdef HAVE_LIBZ
/* return size of unzip data or 0 if failed */
static guint uncompress_data(void *uncompressed_buffer, guint uncompressed_size, void *compressed_data, guint compressed_size)
{
	z_stream stream;
	int err;

	stream.next_in = compressed_data;
	stream.avail_in = compressed_size;
	stream.next_out = uncompressed_buffer;
	stream.avail_out = uncompressed_size;
	stream.zalloc = (alloc_func)0;
	stream.zfree = (free_func)0;
	stream.opaque = (voidpf)0;

	/* negative windowBits to inflateInit2 means "no header" */
	if ((err = inflateInit2(&stream, -MAX_WBITS)) != Z_OK) {
		g_warning("%s(): inflateInit2 failed", __PRETTY_FUNCTION__);
		return 0;
	}

	err = inflate(&stream, Z_FINISH);
	if ((err != Z_OK) && (err != Z_STREAM_END) && stream.msg) {
		g_warning("%s() inflate failed err=%d \"%s\"", __PRETTY_FUNCTION__, err, stream.msg == NULL ? "unknown" : stream.msg);
		inflateEnd(&stream);
		return 0;
	}

	inflateEnd(&stream);
	return(stream.total_out);
}
#endif

/**
 * unzip_file:
 * @zip_file:   pointer to start of compressed data
 * @unzip_size: the size of the compressed data block
 *
 * Returns a pointer to uncompressed data (maybe NULL)
 */
void *unzip_file(gchar *zip_file, gulong *unzip_size)
{
	void *unzip_data = NULL;
#ifndef HAVE_LIBZ
	goto end;
#else
	gchar *zip_data;
	struct _lfh {
		guint32 sig;
		guint16 extract_version;
		guint16 flags;
		guint16 comp_method;
		guint16 time;
		guint16 date;
		guint32 crc_32;
		guint32 compressed_size;
		guint32 uncompressed_size;
		guint16 filename_len;
		guint16 extra_field_len;
	}  __attribute__ ((__packed__)) *local_file_header = NULL;


	local_file_header = (struct _lfh *) zip_file;
	if (GUINT32_FROM_LE(local_file_header->sig) != 0x04034b50) {
		g_warning("%s(): wrong format", __PRETTY_FUNCTION__);
		g_free(unzip_data);
		goto end;
	}

	zip_data = zip_file + sizeof(struct _lfh)
		+ GUINT16_FROM_LE(local_file_header->filename_len)
		+ GUINT16_FROM_LE(local_file_header->extra_field_len);
	gulong uncompressed_size = GUINT32_FROM_LE(local_file_header->uncompressed_size);
	unzip_data = g_malloc(uncompressed_size);

	if (!(*unzip_size = uncompress_data(unzip_data, uncompressed_size, zip_data, GUINT32_FROM_LE(local_file_header->compressed_size)))) {
		g_free(unzip_data);
		unzip_data = NULL;
		goto end;
	}

#endif
end:
	return(unzip_data);
}

/**
 * uncompress_bzip2:
 * @name: The name of the file to attempt to decompress
 *
 * Returns: The name of the uncompressed file (in a temporary location) or NULL
 *   free the returned name after use.
 *
 * Also see: http://www.bzip.org/1.0.5/bzip2-manual-1.0.5.html
 */
gchar* uncompress_bzip2 ( gchar *name )
{
#ifdef HAVE_BZLIB_H
	FILE *ff = g_fopen ( name, "r" );
	if ( !ff )
		return NULL;

	int     bzerror;
	BZFILE* bf = BZ2_bzReadOpen ( &bzerror, ff, 0, 0, NULL, 0 ); // This should take care of the bz2 file header
	if ( bzerror != BZ_OK ) {
		BZ2_bzReadClose ( &bzerror, bf );
		// handle error
		return NULL;
	}

	GFileIOStream *gios;
	GError *error = NULL;
	GFile *gf = g_file_new_tmp ( "vik-bz2-tmp.XXXXXX", &gios, &error );
	gchar *tmpname = g_file_get_path (gf);

	GOutputStream *gos = g_io_stream_get_output_stream ( G_IO_STREAM(gios) );

	// Process in arbitary sized chunks
	char buf[4096];
	bzerror = BZ_OK;
	int nBuf;
	// Now process the actual compression data
	while ( bzerror == BZ_OK ) {
		nBuf = BZ2_bzRead ( &bzerror, bf, buf, 4096 );
		if ( bzerror == BZ_OK || bzerror == BZ_STREAM_END) {
			// do something with buf[0 .. nBuf-1]
			if ( g_output_stream_write ( gos, buf, nBuf, NULL, &error ) < 0 ) {
				g_critical ( "Couldn't write bz2 tmp %s file due to %s", tmpname, error->message );
				g_error_free (error);
				BZ2_bzReadClose ( &bzerror, bf );
				goto end;
			}
		}
	}
	if ( bzerror != BZ_STREAM_END ) {
		BZ2_bzReadClose ( &bzerror, bf );
		// handle error...
		goto end;
	} else {
		BZ2_bzReadClose ( &bzerror, bf );
		g_output_stream_close ( gos, NULL, &error );
	}

 end:
	return tmpname;
#else
	return NULL;
#endif
}
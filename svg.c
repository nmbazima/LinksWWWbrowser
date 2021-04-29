#include "cfg.h"

#ifdef G
#include "links.h"

#ifdef HAVE_SVG

#include <cairo.h>
#include <librsvg/rsvg.h>
#ifdef HAVE_LIBRSVG_RSVG_CAIRO_H
#include <librsvg/rsvg-cairo.h>
#endif
#ifdef HAVE_LIBRSVG_LIBRSVG_FEATURES_H
#include <librsvg/librsvg-features.h>
#endif

#if !defined(LIBRSVG_CHECK_VERSION)
#define LIBRSVG_CHECK_VERSION(a,b,c)	0
#endif

#if LIBRSVG_CHECK_VERSION(2,40,0) && !LIBRSVG_CHECK_VERSION(2,40,16)
#define CURRENTCOLOR_HACK
#endif

#include "bits.h"

struct svg_decoder {
	RsvgHandle *handle;
	unsigned char *buffer;
	int len;
};

void svg_start(struct cached_image *cimg)
{
	struct svg_decoder *deco;

	wait_for_fontconfig();

	{
		static unsigned rsvg_initialized = 0;
		if (!rsvg_initialized) {
#if !defined(GLIB_DEPRECATED_IN_2_36)
			g_type_init();
#endif
#if !LIBRSVG_CHECK_VERSION(2,36,0)
			rsvg_init();
#endif
			rsvg_initialized = 1;
		}
	}

	deco = mem_alloc(sizeof(struct svg_decoder));
	deco->buffer = init_str();
	deco->len = 0;

	cimg->decoder = deco;
	deco->handle = rsvg_handle_new();
}

static int write_data(struct svg_decoder *deco, unsigned char *data, int length)
{
	GError *er = NULL;
	int h1 = close_std_handle(1);
	int h2 = close_std_handle(2);
#ifdef HAVE_RSVG_HANDLE_READ_STREAM_SYNC
	GInputStream *is = g_memory_input_stream_new_from_data((const void *)data, length, NULL);
	if (!is)
		return -1;
	if (!rsvg_handle_read_stream_sync(deco->handle, is, NULL, &er)) {
		g_error_free(er);
		restore_std_handle(1, h1);
		restore_std_handle(2, h2);
		return -1;
	}
#else
	if (!rsvg_handle_write(deco->handle, (const guchar *)data, length, &er)) {
		g_error_free(er);
		restore_std_handle(1, h1);
		restore_std_handle(2, h2);
		return -1;
	}
#endif
	restore_std_handle(1, h1);
	restore_std_handle(2, h2);

	return 0;
}

void svg_restart(struct cached_image *cimg, unsigned char *data, int length)
{
	struct svg_decoder *deco = (struct svg_decoder *)cimg->decoder;
	add_bytes_to_str(&deco->buffer, &deco->len, data, length);
}

static void svg_hack_buffer(struct svg_decoder *deco)
{
#ifdef CURRENTCOLOR_HACK
#define find_string	"\"currentColor\""
#define replace_string	"\"black\""
	unsigned char *new_buffer = init_str();
	int new_len = 0;
	unsigned char *ptr = deco->buffer;
	while (1) {
		int remaining = (int)(deco->buffer + deco->len - ptr);
		unsigned char *f = memmem(ptr, remaining, find_string, strlen(cast_const_char find_string));
		if (!f) {
			if (!new_len) {
				mem_free(new_buffer);
				return;
			}
			add_bytes_to_str(&new_buffer, &new_len, ptr, remaining);
			break;
		} else {
			add_bytes_to_str(&new_buffer, &new_len, ptr, f - ptr);
			add_to_str(&new_buffer, &new_len, cast_uchar replace_string);
			ptr = f + strlen(cast_const_char find_string);
		}
	}
	mem_free(deco->buffer);
	deco->buffer = new_buffer;
	deco->len = new_len;
#endif
}

void svg_finish(struct cached_image *cimg)
{
	struct svg_decoder *deco = (struct svg_decoder *)cimg->decoder;
	RsvgDimensionData dim;
	cairo_surface_t *surf;
	cairo_t *cairo;
	unsigned char *end_buffer, *p;
	int h1, h2;

	svg_hack_buffer(deco);
	if (write_data(deco, deco->buffer, deco->len))
		goto end;

#ifndef HAVE_RSVG_HANDLE_READ_STREAM_SYNC
	{
		h1 = close_std_handle(1);
		h2 = close_std_handle(2);
		GError *er = NULL;
		if (!rsvg_handle_close(deco->handle, &er)) {
			g_error_free(er);
			restore_std_handle(1, h1);
			restore_std_handle(2, h2);
			goto end;
		}
		restore_std_handle(1, h1);
		restore_std_handle(2, h2);
	}
#endif

	rsvg_handle_get_dimensions(deco->handle, &dim);

	cimg->width = dim.width;
	cimg->height = dim.height;
	if (strstr(cast_const_char cimg->url, "/media/math/render/svg/")) {
		cimg->width = (ssize_t)(cimg->width * 1.36);
		cimg->height = (ssize_t)(cimg->height * 1.36);
	}
	cimg->buffer_bytes_per_pixel = 4;
	cimg->red_gamma = cimg->green_gamma = cimg->blue_gamma = (float)sRGB_gamma;
	cimg->strip_optimized = 0;
	if (header_dimensions_known(cimg))
		goto end;

	surf = cairo_image_surface_create_for_data(cimg->buffer, CAIRO_FORMAT_ARGB32, (int)cimg->width, (int)cimg->height, (int)(cimg->width * cimg->buffer_bytes_per_pixel));
	if (cairo_surface_status(surf))
		goto end_surface;

	cairo = cairo_create(surf);
	if (cairo_status(cairo))
		goto end_cairo;

	if (dim.width && dim.height)
		cairo_scale(cairo, (double)cimg->width / (double)dim.width, (double)cimg->height / (double)dim.height);

	h1 = close_std_handle(1);
	h2 = close_std_handle(2);
	rsvg_handle_render_cairo(deco->handle, cairo);
	cairo_surface_flush(surf);
	restore_std_handle(1, h1);
	restore_std_handle(2, h2);

	end_buffer = cimg->buffer + cimg->width * cimg->height * cimg->buffer_bytes_per_pixel;
	if (!big_endian) {
		for (p = cimg->buffer; p < end_buffer; p += 4) {
#ifdef t4c
			t4c t = *(t4c *)p;
			t4c r = (t << 16) | (t >> 16);
			*(t4c *)p = (t & 0xff00ff00U) | (r & 0xff00ffU);
#else
			unsigned char c;
			c = p[0];
			p[0] = p[2];
			p[2] = c;
#endif
		}
	} else {
		for (p = cimg->buffer; p < end_buffer; p += 4) {
#ifdef t4c
			t4c t = *(t4c *)p;
			*(t4c *)p = (t << 8) | (t >> 24);
#else
			unsigned char c;
			c = p[0];
			p[0] = p[1];
			p[1] = p[2];
			p[2] = p[3];
			p[3] = c;
#endif
		}
	}

end_cairo:
	cairo_destroy(cairo);
end_surface:
	cairo_surface_destroy(surf);
end:
	img_end(cimg);
}

void svg_destroy_decoder(struct cached_image *cimg)
{
	struct svg_decoder *deco = (struct svg_decoder *)cimg->decoder;
	g_object_unref(deco->handle);
	mem_free(deco->buffer);
}

void add_svg_version(unsigned char **s, int *l)
{
	add_to_str(s, l, cast_uchar "RSVG (");
#ifdef LIBRSVG_MAJOR_VERSION
	add_num_to_str(s, l, LIBRSVG_MAJOR_VERSION);
#endif
#ifdef LIBRSVG_MINOR_VERSION
	add_chr_to_str(s, l, '.');
	add_num_to_str(s, l, LIBRSVG_MINOR_VERSION);
#endif
#ifdef LIBRSVG_MICRO_VERSION
	add_chr_to_str(s, l, '.');
	add_num_to_str(s, l, LIBRSVG_MICRO_VERSION);
#endif
	add_chr_to_str(s, l, ')');
}

#endif

#endif

/* imgcache.c
 * Image cache
 * (c) 2002 Mikulas Patocka
 * This file is a part of the Links program, released under GPL.
 */

#include "cfg.h"

#ifdef G

#include "links.h"

static struct list_head image_cache_ref = { &image_cache_ref, &image_cache_ref };
static struct list_head image_cache_deref = { &image_cache_deref, &image_cache_deref };

#define CACHED_IMAGE_HASH_SIZE	1024
static struct cached_image *image_hash[CACHED_IMAGE_HASH_SIZE];

static unsigned hash_image(int bg, unsigned char *url, int xw, int yw, int xyw_meaning)
{
	unsigned hash = hash_string(url);
	hash = hash * 0x55 + bg;
	hash = hash * 0x55 + xw;
	hash = hash * 0x55 + yw;
	hash = hash * 0x55 + xyw_meaning;
	return hash % CACHED_IMAGE_HASH_SIZE;
}

/* xyw_meaning either MEANING_DIMS or MEANING_AUTOSCALE. */
struct cached_image *find_cached_image(int bg, unsigned char *url, int xw, int yw, int xyw_meaning, int scale, unsigned aspect)
{
	struct cached_image *i;
	unsigned hash = hash_image(bg, url, xw, yw, xyw_meaning);
	if (xw >= 0 && yw >= 0 && xyw_meaning == MEANING_DIMS) {
		/* The xw and yw is already scaled so that scale and
		 * aspect don't matter.
		 */
		for (i = image_hash[hash]; i; i = i->hash_fwdlink) {
			if (i->background_color == bg
				&& !strcmp(cast_const_char i->url, cast_const_char url)
				&& i->wanted_xw == xw
				&& i->wanted_yw == yw
				&& i->wanted_xyw_meaning == xyw_meaning
				) goto hit;
		}
	} else {
		for (i = image_hash[hash]; i; i = i->hash_fwdlink) {
			if (i->background_color == bg
				&& !strcmp(cast_const_char i->url, cast_const_char url)
				&& i->wanted_xw == xw
				&& i->wanted_yw == yw
				&& i->wanted_xyw_meaning == xyw_meaning
				&& i->scale == scale
				&& i->aspect == aspect
				) goto hit;
		}
	}
	return NULL;

hit:
	i->cimg_refcount++;
	del_from_list(i);
	add_to_list(image_cache_ref, i);
	return i;
}

void add_image_to_cache(struct cached_image *ci)
{
	unsigned hash = hash_image(ci->background_color, ci->url, (int)ci->wanted_xw, (int)ci->wanted_yw, ci->wanted_xyw_meaning);
	ci->hash_fwdlink = image_hash[hash];
	if (ci->hash_fwdlink)
		ci->hash_fwdlink->hash_backlink = &ci->hash_fwdlink;
	ci->hash_backlink = &image_hash[hash];
	image_hash[hash] = ci;
	ci->cimg_refcount = 1;
	add_to_list(image_cache_ref, ci);
}

void deref_cached_image(struct cached_image *ci)
{
	if (ci->cimg_refcount <= 0)
		internal_error("deref_cached_image: refcount underflow: %d", ci->cimg_refcount);
	ci->cimg_refcount--;
	if (!ci->cimg_refcount) {
		del_from_list(ci);
		add_to_list(image_cache_deref, ci);
	}
}

static unsigned long image_size(struct cached_image *cimg)
{
	unsigned long siz = sizeof(struct cached_image);
	switch (cimg->state) {
		case 0:
		case 1:
		case 2:
		case 3:
		case 8:
		case 9:
		case 10:
		case 11:
		break;

		case 12:
		case 14:
		siz += (unsigned long)cimg->width * cimg->height * cimg->buffer_bytes_per_pixel;
		if (cimg->bmp_used){
			case 13:
			case 15:
			siz += (unsigned long)cimg->bmp.x * cimg->bmp.y * (drv->depth & 7);
		}
		break;

#ifdef DEBUG
		default:
		fprintf(stderr, "cimg->state=%d\n", cimg->state);
		internal_error("Invalid cimg->state in image_size\n");
		break;
#endif /* #ifdef DEBUG */
	}
	return siz;
}

static int shrink_image_cache(int u)
{
	struct cached_image *i;
	struct list_head *li;
	longlong si = 0;
	int r = 0;
	foreach(struct cached_image, i, li, image_cache_deref) si += image_size(i);
	foreachback(struct cached_image, i, li, image_cache_deref) {
		if (si <= image_cache_size && u == SH_CHECK_QUOTA)
			break;
		li = li->next;
		r = ST_SOMETHING_FREED;
		si -= image_size(i);
		del_from_list(i);
		if (i->hash_fwdlink)
			i->hash_fwdlink->hash_backlink = i->hash_backlink;
		*i->hash_backlink = i->hash_fwdlink;
		img_destruct_cached_image(i);
		if (u == SH_FREE_SOMETHING) break;
	}
	return r | (list_empty(image_cache_deref) && list_empty(image_cache_ref) ? ST_CACHE_EMPTY : 0);
}

my_uintptr_t imgcache_info(int type)
{
	struct cached_image *i;
	struct list_head *li;
	my_uintptr_t n = 0;
	foreach(struct cached_image, i, li, image_cache_ref) {
		switch (type) {
			case CI_BYTES:
				n += image_size(i);
				break;
			case CI_LOCKED:
			case CI_FILES:
				n++;
				break;
			default:
				internal_error("imgcache_info: query %d", type);
		}
	}
	foreach(struct cached_image, i, li, image_cache_deref) {
		switch (type) {
			case CI_BYTES:
				n += image_size(i);
				break;
			case CI_LOCKED:
				break;
			case CI_FILES:
				n++;
				break;
			default:
				internal_error("imgcache_info: query %d", type);
		}
	}
	return n;
}

void init_imgcache(void)
{
	unsigned i;
	register_cache_upcall(shrink_image_cache, MF_GPI, cast_uchar "imgcache");
	for (i = 0; i < CACHED_IMAGE_HASH_SIZE; i++)
		image_hash[i] = NULL;
}

#endif

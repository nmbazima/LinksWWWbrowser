#include "cfg.h"

#if defined(G) && defined(HAVE_FREETYPE)

#include "links.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#ifdef FT_BITMAP_H
#include FT_BITMAP_H
#endif

#ifdef FT_LOAD_TARGET_LCD
#define select_lcd(x, y)	(!display_optimize ? (x) : (y))
#define ret_if_lcd		do { } while (0)
#else
#define select_lcd(x, y)	(x)
#define ret_if_lcd		if (display_optimize) return -1
#endif

struct freetype_face {
	unsigned char *path;
	FT_Face face;
	float baseline;
	int allocated;
};

struct freetype_face faces[FF_SHAPES];

static int ft_initialized = 0;
static FT_Library library;

struct lru_entry **freetype_cache = NULL;

struct metric_cache_entry {
	struct freetype_face *face;
	int char_number;
	int y;
	unsigned char flags;
	int x;
};

#define sizeof_freetype_metric_cache	65536
static struct metric_cache_entry *freetype_metric_cache = NULL;

struct freetype_face *freetype_flags_to_face(int fflags)
{
	fflags &= FF_SHAPES - 1;
	if (faces[fflags].path)
		return &faces[fflags];
	return NULL;
}

static void cleanup_state(struct freetype_face *face)
{
	if (face->path) {
		mem_free(face->path);
		FT_Done_Face(face->face);
	}
	memset(face, 0, sizeof(struct freetype_face));
	memset(freetype_metric_cache, 0, sizeof_freetype_metric_cache * sizeof(struct metric_cache_entry));
}

static void calculate_baseline(struct freetype_face *face)
{
	float val;
	int h = face->face->height;
	int asc = face->face->ascender;
	int desc = -face->face->descender;
	int ad = asc + desc;
	if (ad > h)
		h = ad;
	if (!h)
		h = 1;
	val = (desc + (float)(h - ad) / 2) / h;
	/*fprintf(stderr, "calculat_baseline: %f %f\n", val, 1 / val);*/
	face->baseline = val;
}

static void freetype_setup_font(struct freetype_face *face, unsigned char *path)
{
	unsigned char *idx_ptr;
	long idx;

	if (!ft_initialized)
		return;

	if (!freetype_cache)
		freetype_cache = mem_calloc(sizeof_freetype_cache * sizeof(struct lru_entry *));

	if (!freetype_metric_cache)
		freetype_metric_cache = mem_calloc(sizeof_freetype_metric_cache * sizeof(struct metric_cache_entry));

	cleanup_state(face);

	if (!path || !*path)
		return;

	face->path = stracpy(path);
	idx = 0;

	idx_ptr = cast_uchar strrchr(cast_const_char face->path, '@');
	if (idx_ptr && idx_ptr != face->path && idx_ptr[-1] == ' ' && idx_ptr[1]) {
		char *end;
		idx = strtol(cast_const_char (idx_ptr + 1), &end, 10);
		if (!*end) {
			idx_ptr[-1] = 0;
		} else {
			idx = 0;
		}

	}

	if (FT_New_Face(library, cast_const_char path, idx, &face->face)) {
		mem_free(face->path);
		face->path = NULL;
		return;
	}

	calculate_baseline(face);

	/*fprintf(stderr, "%d %d %d", face->face->height, face->face->ascender, face->face->descender);*/
}

struct freetype_face *freetype_get_font(unsigned char *path)
{
	struct freetype_face *face = mem_calloc(sizeof(struct freetype_face));
	freetype_setup_font(face, path);
	if (!face->path) {
		mem_free(face);
		return NULL;
	}
	face->allocated = 1;
	return face;
}

void freetype_free_font(struct freetype_face *face)
{
	if (face->allocated) {
		cleanup_state(face);
		mem_free(face);
	}
}

unsigned char *freetype_get_allocated_font_name(struct style *st)
{
	if (st->ft_face && st->ft_face->allocated)
		return stracpy(st->ft_face->path);
	return NULL;
}

static inline int get_baseline(struct freetype_face *face, int y)
{
	return (int)(y * face->baseline + 0.5F);
}

static int freetype_load_metric(struct style *st, int char_number, int *xp, int y)
{
	FT_Face face = st->ft_face->face;
	FT_GlyphSlot slot = face->glyph;
	FT_Error err;
	FT_UInt glyph_index;
	int baseline = get_baseline(st->ft_face, y);

	ret_if_lcd;

	if ((err = FT_Set_Pixel_Sizes(face, 0, y - baseline))) {
		/*error("FT_Set_Pixel_Sizes failed: %d", err);*/
		return -1;
	}

	glyph_index = FT_Get_Char_Index(face, char_number);
	if (!glyph_index) {
		/*fprintf(stderr, "missing glyph: %d\n", char_number);*/
		return -1;
	}

	if ((err = FT_Load_Glyph(face, glyph_index, select_lcd(FT_LOAD_TARGET_NORMAL, FT_LOAD_TARGET_LCD)))) {
		error("FT_Load_Glyph failed: %d", err);
		return -1;
	}

	slot = face->glyph;

	*xp = (int)(slot->advance.x >> 6);

	if (*xp <= 0)
		*xp = 1;

	return 0;
}

int freetype_load_metric_cached(struct style *st, int char_number, int *xp, int y)
{
	int cache_slot;
	struct metric_cache_entry *entry;

	cache_slot = (char_number + y + (y << 12)) & (sizeof_freetype_metric_cache - 1);
	entry = &freetype_metric_cache[cache_slot];

	if (entry->face == st->ft_face && entry->char_number == char_number && entry->y == y && entry->flags == st->flags) {
		*xp = entry->x;
		return 0;
	}

	if (freetype_load_metric(st, char_number, xp, y))
		return -1;

	entry->face = st->ft_face;
	entry->char_number = char_number;
	entry->y = y;
	entry->flags = st->flags;
	entry->x = *xp;

	return 0;
}

int freetype_type_character(struct style *st, int char_number, unsigned char **dest, int *xp, int y)
{
	FT_Face face;
	FT_GlyphSlot slot;
	FT_Bitmap *bitmap;
#ifdef FT_BITMAP_H
	FT_Bitmap converted_bitmap;
#endif
	FT_Error err;
	int j, x;
	int start_x, start_y;
	int len_x, len_y;
	int offset_x, offset_y;
	unsigned char *s, *d;
	int baseline;

	face = st->ft_face->face;
	slot = face->glyph;
	baseline = get_baseline(st->ft_face, y);

	ret_if_lcd;

	if (freetype_load_metric(st, char_number, xp, y))
		return -1;

	if (st->mono_space >= 0) {
		*xp = st->mono_space;
	}

	slot = face->glyph;
	if ((err = FT_Render_Glyph(slot, select_lcd(FT_RENDER_MODE_NORMAL, FT_RENDER_MODE_LCD)))) {
		/*error("FT_Render_Glyph failed: %d", err);*/
		return -1;
	}

	if (slot->bitmap.pixel_mode == select_lcd(FT_PIXEL_MODE_GRAY, FT_PIXEL_MODE_LCD)) {
		bitmap = &slot->bitmap;
	} else {
#ifdef FT_BITMAP_H
		FT_Bitmap_New(&converted_bitmap);
		if (FT_Bitmap_Convert(library, &slot->bitmap, &converted_bitmap, 1)) {
			FT_Bitmap_Done(library, &converted_bitmap);
			error("FT_Bitmap_Convert failed");
			return -1;
		}
		bitmap = &converted_bitmap;
#else
		return -1;
#endif
	}

	if (display_optimize)
		*xp *= 3;

	x = *xp;
	offset_x = -slot->bitmap_left;
	offset_y = -y + baseline + slot->bitmap_top;

	if (offset_x >= 0) {
		if (offset_x >= (int)bitmap->width) {
			start_x = len_x = 0;
		} else if (offset_x + x >= (int)bitmap->width) {
			start_x = offset_x;
			len_x = (int)bitmap->width - offset_x;
		} else {
			start_x = offset_x;
			len_x = x;
		}
	} else {
		if (offset_x + x <= 0) {
			start_x = len_x = 0;
		} else if (offset_x + x <= (int)bitmap->width) {
			start_x = 0;
			len_x = x + offset_x;
		} else {
			start_x = 0;
			len_x = (int)bitmap->width;
		}
	}
	if (offset_y >= 0) {
		if (offset_y >= (int)bitmap->rows) {
			start_y = len_y = 0;
		} else if (offset_y + y >= (int)bitmap->rows) {
			start_y = offset_y;
			len_y = (int)bitmap->rows - offset_y;
		} else {
			start_y = offset_y;
			len_y = y;
		}
	} else {
		if (offset_y + y <= 0) {
			start_y = len_y = 0;
		} else if (offset_y + y <= (int)bitmap->rows) {
			start_y = 0;
			len_y = y + offset_y;
		} else {
			start_y = 0;
			len_y = (int)bitmap->rows;
		}
	}

#if 0
	fprintf(stderr, "advance: x = %ld, y = %ld\n", slot->advance.x >> 6, slot->advance.y >> 6);
	fprintf(stderr, "%d x %d\n", bitmap->width, bitmap->rows);
	fprintf(stderr, "x/y: %d %d, start_x/start_y %d %d, len_x/len_y %d %d\n", x, y, start_x, start_y, len_x, len_y);
	fprintf(stderr, "bitmap_left, bitmap_top %d - %d\n", slot->bitmap_left, slot->bitmap_top);
	fprintf(stderr, "\n");
#endif

	if ((unsigned)x * (unsigned)y / (unsigned)y != (unsigned)x ||
	    (unsigned)x * (unsigned)y > MAXINT) overalloc();
	*dest = mem_calloc(x * y);

	s = bitmap->buffer + bitmap->pitch * start_y + start_x;
	d = *dest + (offset_y > 0 ? 0 : x * -offset_y) + (offset_x > 0 ? 0 : -offset_x);
	for (j = 0; j < len_y; j++) {
		memcpy(d, s, len_x);
		s += bitmap->pitch;
		d += x;
	}

#ifdef FT_BITMAP_H
	if (bitmap == &converted_bitmap)
		FT_Bitmap_Done(library, &converted_bitmap);
#endif

	return 0;
}


void freetype_init(void)
{
	FT_Error err;

	memset(&faces, 0, sizeof faces);
	if ((err = FT_Init_FreeType(&library))) {
		error("FT_Init_FreeType failed: %d", err);
		return;
	}
	ft_initialized = 1;

	freetype_reload();
}

void freetype_reload(void)
{
	freetype_setup_font(&faces[0], font_file);
	freetype_setup_font(&faces[FF_BOLD], font_file_b);
	freetype_setup_font(&faces[FF_MONOSPACED], font_file_m);
	freetype_setup_font(&faces[FF_MONOSPACED | FF_BOLD], font_file_m_b);

#ifdef USE_ITALIC
	freetype_setup_font(&faces[FF_ITALIC], font_file_i);
	freetype_setup_font(&faces[FF_ITALIC | FF_BOLD], font_file_i_b);
	freetype_setup_font(&faces[FF_ITALIC | FF_MONOSPACED], font_file_i_m);
	freetype_setup_font(&faces[FF_ITALIC | FF_MONOSPACED | FF_BOLD], font_file_i_m_b);
#endif
}

void freetype_done(void)
{
	FT_Error err;
	int i;

	for (i = 0; i < FF_SHAPES; i++)
		freetype_setup_font(&faces[i], NULL);

	if (freetype_cache)
		mem_free(freetype_cache), freetype_cache = NULL;
	if (freetype_metric_cache)
		mem_free(freetype_metric_cache), freetype_metric_cache = NULL;

	if (ft_initialized) {
		if ((err = FT_Done_FreeType(library)))
			error("FT_Done_FreeType failed: %d", err);
		ft_initialized = 0;
	}
}


void add_freetype_version(unsigned char **s, int *l)
{
#ifdef HAVE_FT_LIBRARY_VERSION
	FT_Int v1, v2, v3;
	int initialized = ft_initialized;

	if (!initialized) {
		freetype_init();
	}

	FT_Library_Version(library, &v1, &v2, &v3);

	if (!initialized) {
		freetype_done();
	}

	add_to_str(s, l, cast_uchar "FreeType ");
	add_num_to_str(s, l, v1);
	add_chr_to_str(s, l, '.');
	add_num_to_str(s, l, v2);
	add_chr_to_str(s, l, '.');
	add_num_to_str(s, l, v3);
#else
	add_to_str(s, l, cast_uchar "FreeType ");
	add_num_to_str(s, l, FREETYPE_MAJOR);
	add_chr_to_str(s, l, '.');
	add_num_to_str(s, l, FREETYPE_MINOR);
#ifdef FREETYPE_PATCH
	add_chr_to_str(s, l, '.');
	add_num_to_str(s, l, FREETYPE_PATCH);
#endif
#endif
}

#endif

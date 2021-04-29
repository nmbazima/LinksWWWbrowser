#include "cfg.h"

#if defined(G) && defined(HAVE_FREETYPE)

#include "links.h"

#include <fontconfig/fontconfig.h>

#if defined(__CYGWIN__)
#ifdef HAVE_PTHREADS
#define BACKGROUND_FONT_INIT
#endif
#include <windows.h>
#include <w32api/shlobj.h>
static void set_font_path(void)
{
	unsigned char *path = cast_uchar "/cygdrive/c/Windows/Fonts";
#if defined(HAVE_CYGWIN_CONV_PATH)
	unsigned char win32_path[MAX_PATH];
	if (SHGetFolderPathA(NULL, CSIDL_FONTS, NULL, 0, cast_char win32_path) == S_OK) {
		ssize_t l;
		unsigned char *cyg_path;
		l = cygwin_conv_path(CCP_WIN_A_TO_POSIX, win32_path, NULL, 0);
		if (l <= 0)
			goto do_default;
		cyg_path = alloca(l);
		l = cygwin_conv_path(CCP_WIN_A_TO_POSIX, win32_path, cyg_path, l);
		if (l < 0)
			goto do_default;
		path = cyg_path;
	}
	do_default:
#endif
	FcConfigAppFontAddDir(NULL, path);
}
#else
#define set_font_path()	do { } while (0)
#endif


static void *do_initialize_fontconfig(void *ptr)
{
	if (!FcInit())
		fatal_exit("FcInit returned an error");
	set_font_path();
	return NULL;
}


static unsigned fontconfig_initialized = 0;

#ifdef BACKGROUND_FONT_INIT

#include <pthread.h>

static pthread_t font_thread;

void wait_for_fontconfig(void)
{
	if (!fontconfig_initialized) {
		int r;
		if ((r = pthread_join(font_thread, NULL)))
			fatal_exit("pthread_join failed: %s", strerror(r));
		fontconfig_initialized = 1;
	}
}

void fontconfig_init(void)
{
	int r;
	if ((r = pthread_create(&font_thread, NULL, do_initialize_fontconfig, NULL)))
		fatal_exit("Could not start thread: %s", strerror(r));
}

#else

void wait_for_fontconfig(void)
{
	if (!fontconfig_initialized) {
		do_initialize_fontconfig(NULL);
		fontconfig_initialized = 1;
	}
}

void fontconfig_init(void)
{
}

#endif

LIBC_CALLBACK static int compare_fonts(const void *f1_, const void *f2_)
{
	struct list_of_fonts *f1 = (struct list_of_fonts *)f1_;
	struct list_of_fonts *f2 = (struct list_of_fonts *)f2_;
	int c = strcmp(cast_const_char f1->name, cast_const_char f2->name);
	if (c) return c;
	return strcmp(cast_const_char f1->file, cast_const_char f2->file);
}

void fontconfig_list_fonts(struct list_of_fonts **fonts, int *n_fonts, int monospaced)
{
	int i, j;

	FcPattern *pat;
	FcValue val;
	FcObjectSet *os;
	FcFontSet *fs;

	wait_for_fontconfig();

	*fonts = DUMMY;
	*n_fonts = 0;

	pat = FcPatternCreate();
	if (!pat) {
		error("FcPatternCreate failed");
		return;
	}

	if (monospaced) {
		val.type = FcTypeInteger;
		val.u.i = FC_MONO;
		if (!FcPatternAdd(pat, FC_SPACING, val, FcFalse))
			error("FcPatternAdd FC_SPACING failed");
	}

	val.type = FcTypeBool;
	val.u.b = FcTrue;
	if (!FcPatternAdd(pat, FC_SCALABLE, val, FcFalse))
		error("FcPatterAdd FC_SCALABLE failed");

	os = FcObjectSetBuild(FC_FILE, FC_INDEX, FC_FAMILY, FC_STYLE,
#ifdef FC_FULLNAME
		FC_FULLNAME,
#endif
		(char *)NULL);
	if (!os) {
		error("FcObjectSetBuild failed");
		goto free_pattern_ret;
	}
	fs = FcFontList(0, pat, os);
	if (!fs) {
		error("FcFontList failed");
		goto free_object_set_ret;
	}

	if ((unsigned)fs->nfont > MAXINT / sizeof(struct list_of_fonts) - 1) overalloc();
	*fonts = mem_alloc((fs->nfont + 1) * sizeof(struct list_of_fonts));

	for (i = 0; i < fs->nfont; i++) {
		FcResult res;
		unsigned char *value;
		int index;
		unsigned char *file, *name;
		int file_l, name_l;
		FcPattern *font = fs->fonts[i];

		file = init_str(), file_l = 0;
		name = init_str(), name_l = 0;

		res = FcPatternGetString(font, FC_FILE, 0, &value);
		if (res != FcResultMatch) {
cont:
			mem_free(file);
			mem_free(name);
			continue;
		}
		add_to_str(&file, &file_l, value);
		res = FcPatternGetInteger(font, FC_INDEX, 0, &index);
		if (res == FcResultMatch && index) {
			add_to_str(&file, &file_l, cast_uchar " @");
			add_num_to_str(&file, &file_l, index);
		}

		if (file_l >= MAX_STR_LEN)
			goto cont;

#ifdef FC_FULLNAME
		res = FcPatternGetString(font, FC_FULLNAME, 0, &value);
		if (res == FcResultMatch) {
			add_to_str(&name, &name_l, value);
		} else
#endif
		{
			unsigned char *family, *style;
			res = FcPatternGetString(font, FC_FAMILY, 0, &family);
			if (res != FcResultMatch) goto cont;
			res = FcPatternGetString(font, FC_STYLE, 0, &style);
			if (res != FcResultMatch) goto cont;

			add_to_str(&name, &name_l, family);
			add_chr_to_str(&name, &name_l, ' ');
			add_to_str(&name, &name_l, style);
		}

		(*fonts)[*n_fonts].file = file;
		(*fonts)[*n_fonts].name = name;
		(*n_fonts)++;

		/*FcPatternPrint(font);*/
	}

	if (*n_fonts) qsort(*fonts, *n_fonts, sizeof(struct list_of_fonts), (int (*)(const void *, const void *))compare_fonts);

	for (i = 1, j = 1; i < *n_fonts; i++) {
		if (strcmp(cast_const_char (*fonts)[j - 1].name, cast_const_char (*fonts)[i].name)) {
			(*fonts)[j++] = (*fonts)[i];
		} else {
			mem_free((*fonts)[i].name);
			mem_free((*fonts)[i].file);
		}
	}

	*n_fonts = j;
	(*fonts)[j].file = NULL;
	(*fonts)[j].name = NULL;

	/*{
		for (i = 0; i < *n_fonts; i++) {
			fprintf(stderr, "%d: '%s' '%s'\n", i, (*fonts)[i].file, (*fonts)[i].name);
		}
	}*/

	FcFontSetDestroy(fs);
free_object_set_ret:
	FcObjectSetDestroy(os);
free_pattern_ret:
	FcPatternDestroy(pat);
}

void fontconfig_free_fonts(struct list_of_fonts *fonts)
{
	int i;
	for (i = 0; fonts[i].file; i++) {
		mem_free(fonts[i].file);
		mem_free(fonts[i].name);
	}
	mem_free(fonts);
}

void add_fontconfig_version(unsigned char **s, int *l)
{
	int fc_version;
#ifdef HAVE_FT_LIBRARY_VERSION
	fc_version = FcGetVersion();
#else
	fc_version = FC_VERSION;
#endif
	add_to_str(s, l, cast_uchar "FontConfig ");
	add_num_to_str(s, l, fc_version / 10000);
	add_chr_to_str(s, l, '.');
	add_num_to_str(s, l, fc_version / 100 % 100);
	add_chr_to_str(s, l, '.');
	add_num_to_str(s, l, fc_version % 100);
}

#endif

/* language.c
 * (c) 2002 Mikulas Patocka
 * This file is a part of the Links program, released under GPL.
 */

#include "links.h"

struct translation {
	const char *name;
};

struct translation_desc {
	const struct translation *t;
};

unsigned char dummyarray[T__N_TEXTS];

#include "language.inc"

static unsigned char **translation_array[N_LANGUAGES][N_CODEPAGES];

int current_language;
static int current_lang_charset;

void init_trans(void)
{
	int i, j;
	for (i = 0; i < N_LANGUAGES; i++)
		for (j = 0; j < N_CODEPAGES; j++)
			translation_array[i][j] = NULL;
	set_language(-1);
}

void shutdown_trans(void)
{
	int i, j, k;
	for (i = 0; i < N_LANGUAGES; i++)
		for (j = 0; j < N_CODEPAGES; j++) if (translation_array[i][j]) {
			for (k = 0; k < T__N_TEXTS; k++) {
				unsigned char *txt = translation_array[i][j][k];
				if (txt &&
				    txt != cast_uchar translations[i].t[k].name &&
				    txt != cast_uchar translation_english[k].name)
					mem_free(txt);
			}
			mem_free(translation_array[i][j]);
		}
}

int get_language_from_lang(unsigned char *lang)
{
	unsigned char *p;
	int i;
	lang = stracpy(lang);
	lang[strcspn(cast_const_char lang, ".@")] = 0;
	if (!casestrcmp(lang, cast_uchar "nn_NO"))
		strcpy(cast_char lang, "no");
	for (p = lang; *p; p++) {
		if (*p >= 'A' && *p <= 'Z')
			*p += 'a' - 'A';
		if (*p == '_')
			*p = '-';
	}
search_again:
	for (i = 0; i < n_languages(); i++) {
		p = cast_uchar translations[i].t[T__ACCEPT_LANGUAGE].name;
		if (!p)
			continue;
		p = stracpy(p);
		p[strcspn(cast_const_char p, ",;")] = 0;
		if (!casestrcmp(lang, p)) {
			mem_free(p);
			mem_free(lang);
			return i;
		}
		mem_free(p);
	}
	if ((p = cast_uchar strchr(cast_const_char lang, '-'))) {
		*p = 0;
		goto search_again;
	}
	mem_free(lang);
	return -1;
}

int get_default_language(void)
{
	static int default_language = -1;
	unsigned char *lang;

	if (default_language >= 0)
		return default_language;

	default_language = os_default_language();
	if (default_language >= 0)
		return default_language;

	lang = cast_uchar getenv("LANG");
	if (lang) {
		default_language = get_language_from_lang(lang);
		if (default_language >= 0)
			return default_language;
	}

	default_language = get_language_from_lang(cast_uchar "en");
	if (default_language < 0)
		internal_error("default language 'english' not found");
	return default_language;
}

int get_current_language(void)
{
	if (current_language >= 0)
		return current_language;
	return get_default_language();
}

int get_default_charset(void)
{
	static int default_charset = -1;
	unsigned char *lang, *p;
	int i;
	if (default_charset >= 0)
		return default_charset;

	i = os_default_charset();
	if (i >= 0)
		goto ret_i;

	lang = cast_uchar getenv("LC_CTYPE");
	if (!lang)
		lang = cast_uchar getenv("LANG");
	if (!lang) {
		i = 0;
		goto ret_i;
	}
	if ((p = cast_uchar strchr(cast_const_char lang, '.'))) {
		p++;
	} else if ((i = get_cp_index(lang)) >= 0) {
		goto ret_i;
	} else {
		if (strlen(cast_const_char lang) > 5 && !casestrcmp(cast_uchar (strchr(cast_const_char lang, 0) - 5), cast_uchar "@euro")) {
			p = cast_uchar "ISO-8859-15";
		} else {
			int def_lang = get_language_from_lang(lang);
			if (def_lang < 0) {
				i = 0;
				goto ret_i;
			}
			p = cast_uchar translations[def_lang].t[T__DEFAULT_CHAR_SET].name;
			if (!p)
				p = cast_uchar "";
		}
	}
	i = get_cp_index(p);

	if (i < 0)
		i = 0;

	ret_i:
#ifndef ENABLE_UTF8
	if (!F && i == utf8_table)
		i = 0;
#endif
	default_charset = i;
	return i;
}

int get_commandline_charset(void)
{
	return dump_codepage == -1 ? get_default_charset() : dump_codepage;
}

static inline int get_text_index(unsigned char *text)
{
	unsigned char * volatile dmv = dummyarray;
	unsigned char * dm = dmv;
	if (((my_uintptr_t)text >= (my_uintptr_t)dm && (my_uintptr_t)text < (my_uintptr_t)(dm + T__N_TEXTS)))
		return (int)(text - dm);
	return -1;
}

unsigned char *get_text_translation(unsigned char *text, struct terminal *term)
{
	unsigned char **current_tra;
	unsigned char *trn;
	int charset;
	int language_idx = get_current_language();
	int idx;
	if (!term) charset = 0;
	else charset = term_charset(term);
	idx = get_text_index(text);
	if (idx < 0) return text;
	if ((current_tra = translation_array[language_idx][charset])) {
		unsigned char *tt;
		if ((trn = current_tra[idx])) return trn;
		tr:
		if (!(tt = cast_uchar translations[language_idx].t[idx].name)) {
			trn = cast_uchar translation_english[idx].name;
		} else {
			struct document_options l_opt;
			memset(&l_opt, 0, sizeof(l_opt));
			l_opt.plain = 0;
			l_opt.cp = charset;
			trn = convert(current_lang_charset, charset, tt, &l_opt);
			if (!strcmp(cast_const_char trn, cast_const_char tt)) {
				mem_free(trn);
				trn = tt;
			}
		}
		current_tra[idx] = trn;
	} else {
		if (current_lang_charset && charset != current_lang_charset) {
			current_tra = translation_array[language_idx][charset] = mem_alloc(sizeof (unsigned char *) * T__N_TEXTS);
			memset(current_tra, 0, sizeof (unsigned char *) * T__N_TEXTS);
			goto tr;
		}
		if (!(trn = cast_uchar translations[language_idx].t[idx].name)) {
			trn = cast_uchar translation_english[idx].name;
		}
	}
	return trn;
}

unsigned char *get_english_translation(unsigned char *text)
{
	int idx = get_text_index(text);
	if (idx < 0) return text;
	return cast_uchar translation_english[idx].name;
}

int n_languages(void)
{
	return N_LANGUAGES;
}

unsigned char *language_name(int l)
{
	if (l == -1) return cast_uchar "default";
	return cast_uchar translations[l].t[T__LANGUAGE].name;
}

void set_language(int l)
{
	int i;
	unsigned char *cp;
	current_language = l;
	l = get_current_language();
	cp = cast_uchar translations[l].t[T__CHAR_SET].name;
	i = get_cp_index(cp);
	if (i == -1) {
		internal_error("Unknown charset for language %s.", translations[l].t[T__LANGUAGE].name);
		i = 0;
	}
	current_lang_charset = i;
}

#if defined(HAVE_SETLOCALE) && defined(LC_CTYPE) && defined(LC_MESSAGES)
#define USE_OLD_LOCALE
#endif
#if defined(HAVE_FREELOCALE) && defined(HAVE_NEWLOCALE) && defined(HAVE_STRERROR_L)
#define USE_NEW_LOCALE
#endif

static unsigned char *strerror_alloc_internal(int err, void *loc_p)
{
	unsigned char *strerror_buf, *e;
	unsigned size = 32;
larger_buf:
	strerror_buf = mem_alloc(size);
	if (!loc_p)
		e = cast_uchar strerror(err);
	else
#ifdef USE_NEW_LOCALE
		e = cast_uchar strerror_l(err, *(locale_t *)loc_p);
#else
		e = NULL;
#endif
	if (!e)
		e = cast_uchar "Unknown error";
	if (strlen(cast_const_char e) >= size) {
		size *= 2;
		if (!size) overalloc();
		mem_free(strerror_buf);
		goto larger_buf;
	}
	strcpy(cast_char strerror_buf, cast_const_char e);
	return strerror_buf;
}

#ifdef USE_OLD_LOCALE

#ifdef USE_NEW_LOCALE

static unsigned char *strerror_try_locale(int err, unsigned char *lc, int charset_from, int charset_to)
{
	locale_t nl;
	unsigned char *str, *str_converted;
	nl = newlocale(LC_CTYPE_MASK | LC_MESSAGES_MASK, cast_const_char lc, (locale_t)0);
	if (nl == (locale_t)0)
		return NULL;
	str = strerror_alloc_internal(err, &nl);
	freelocale(nl);
	str_converted = convert(charset_from, charset_to, str, NULL);
	mem_free(str);
	return str_converted;
}

#else

static unsigned char *strerror_try_locale(int err, unsigned char *lc, int charset_from, int charset_to)
{
	unsigned char *prev_type;
	unsigned char *prev_msg;
	unsigned char *str;
	unsigned char *str_converted = NULL;

	prev_type = cast_uchar setlocale(LC_CTYPE, NULL);
	if (!prev_type)
		goto err0;
	prev_type = stracpy(prev_type);
	prev_msg = cast_uchar setlocale(LC_MESSAGES, NULL);
	if (!prev_msg)
		goto err1;
	prev_msg = stracpy(prev_msg);

	if (!setlocale(LC_CTYPE, cast_const_char lc))
		goto err2;
	if (!setlocale(LC_MESSAGES, cast_const_char lc))
		goto err2;

	str = strerror_alloc_internal(err, NULL);
	str_converted = convert(charset_from, charset_to, str, NULL);
	mem_free(str);

err2:
	setlocale(LC_MESSAGES, cast_const_char prev_msg);
	mem_free(prev_msg);
err1:
	setlocale(LC_CTYPE, cast_const_char prev_type);
	mem_free(prev_type);
err0:
	return str_converted;
}

#endif

static unsigned char *strerror_try_charset(int err, unsigned char *lc, unsigned char *charset_from, int charset_to)
{
	unsigned char *full, *ret;
	int idx;

	idx = get_cp_index(charset_from);
	if (idx < 0)
		return NULL;
	full = stracpy(lc);
	add_to_strn(&full, cast_uchar ".");
	add_to_strn(&full, charset_from);

	ret = strerror_try_locale(err, full, idx, charset_to);

	mem_free(full);
	return ret;
}

static unsigned char *strerror_try_language(int err, unsigned char *lc, unsigned char *lc_charset, int charset_to)
{
#if defined(__GLIBC__) && defined(__GLIBC_MINOR__) && __GLIBC__ == 2 && __GLIBC_MINOR__ < 5
	return strerror_try_charset(err, lc, cast_uchar "UTF-8", charset_to);
#else
	unsigned char *result;
	if (charset_to == utf8_table) {
		result = strerror_try_charset(err, lc, cast_uchar "UTF-8", charset_to);
		if (result)
			return result;
		result = strerror_try_charset(err, lc, lc_charset, charset_to);
		if (result)
			return result;
	} else {
		result = strerror_try_charset(err, lc, lc_charset, charset_to);
		if (result)
			return result;
		result = strerror_try_charset(err, lc, cast_uchar "UTF-8", charset_to);
		if (result)
			return result;
	}
	return NULL;
#endif
}

unsigned char *strerror_alloc(int err, struct terminal *term)
{
	int charset_to;
	unsigned char *lc_str, *lc_charset;
	if (term) {
		charset_to = term_charset(term);
	} else {
		charset_to = 0;
	}
	lc_str = get_text_translation(TEXT_(T__LOCALE_CODE), term);
	lc_charset = get_text_translation(TEXT_(T__DEFAULT_CHAR_SET), term);
	while (*lc_str) {
		int l = (int)strcspn(cast_const_char lc_str, ",");
		unsigned char *lc = memacpy(lc_str, l);
		unsigned char *result;

		result = strerror_try_language(err, lc, lc_charset, charset_to);
		mem_free(lc);
		if (result)
			return result;

		lc_str += l;
		lc_str += *lc_str == ',';
	}
	return strerror_alloc_internal(err, NULL);
}

#else

unsigned char *strerror_alloc(int err, struct terminal *term)
{
	return strerror_alloc_internal(err, NULL);
}

#endif

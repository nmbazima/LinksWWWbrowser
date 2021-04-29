#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <sys/types.h>
#include <png.h>

#define MAX_LETTERS 65536

static FILE *output;

struct letter {
	unsigned unicode;
	unsigned length;
	unsigned x, y;
	unsigned sequence;
};

static struct letter letters_system[MAX_LETTERS];
static struct letter letters_normal[MAX_LETTERS];
static struct letter letters_bold[MAX_LETTERS];
static struct letter letters_monospaced[MAX_LETTERS];
static unsigned n_letters_system;
static unsigned n_letters_normal;
static unsigned n_letters_bold;
static unsigned n_letters_monospaced;

static unsigned global_sequence = 0;

static int compare_letters(const void *l1, const void *l2)
{
	const struct letter *letter1 = l1;
	const struct letter *letter2 = l2;
	return (int)letter1->unicode - (int)letter2->unicode;
}

static void get_png_dimensions(FILE *stream, int *x, int *y)
{
	png_structp png_ptr;
	png_infop info_ptr;

	png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	info_ptr = png_create_info_struct(png_ptr);
	png_init_io(png_ptr, stream);
	png_read_info(png_ptr, info_ptr);
	*x = png_get_image_width(png_ptr,info_ptr);
	*y = png_get_image_height(png_ptr,info_ptr);
	png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
}

static int char_pos=0; /* To not makes lines excessively long. */

/* Returns forbidden_0_to_7 for next char. */
static int print_char(FILE *output, int c, int forbidden_0_to_7)
{
	if (char_pos >= 70){
		fputs("\"\n\"", output);
		char_pos = 0;
	}
	switch (c) {
		case '\n':
			fputs("\\n", output);
two:
			char_pos += 2;
			forbidden_0_to_7 = 0;
			break;

		case '\t':
			fputs("\\t", output); goto two;

		case '\b':
			fputs("\\b", output); goto two;

		case '\r':
			fputs("\\r", output); goto two;

		case '\f':
			fputs("\\f", output); goto two;

		case '\\':
			fputs("\\\\", output); goto two;

		case '\'':
			fputs("\\\'", output); goto two;

		default:
			if (c < ' ' || c== '"' || c=='?' || c > 126 ||
			   (c >= '0' && c <= '7' && forbidden_0_to_7)) {
				fprintf(output, "\\%o", c);
				if (c >= 0100) char_pos += 4;
				else if (c >= 010) char_pos += 3;
				else char_pos += 2;
				forbidden_0_to_7 = c < 0100;
			} else {
				putc(c, output);
				char_pos++;
				forbidden_0_to_7 = 0;

			}
			break;
	}
	return forbidden_0_to_7;
}

static void process_file(FILE *f, struct letter *l)
{
	int c;
	int forbidden_0_to_7;
	int count = 0;
	fprintf(output, "static_const unsigned char letter_%u[] = \"", global_sequence);
	l->sequence = global_sequence;
	char_pos = 38;
	forbidden_0_to_7 = 0;
	while (EOF != (c = fgetc(f))){
		forbidden_0_to_7 = print_char(output, c, forbidden_0_to_7);
		count++;
	}
	fprintf(output, "\";\n\n");
	l->length = ftell(f);
	global_sequence++;
	rewind(f);
	get_png_dimensions(f, &l->x, &l->y);
	/*fprintf(stderr, "%d %d\n", letters[i].x, letters[i].y);*/
}

static void load_letters(struct letter letters[MAX_LETTERS], unsigned *n, char *path)
{
	DIR *d;
	struct dirent *de;
	unsigned i;
	
	*n = 0;
	d = opendir(path);
	if (!d) perror(path), exit(1);

	while ((de = readdir(d))) {
		char *end;
		unsigned unicode = strtoul(de->d_name, &end, 16);
		if (end == de->d_name + 6 && !strcmp(end, ".png")) {
			if (*n >= MAX_LETTERS) fprintf(stderr, "too many letters\n"), exit(1);
			letters[(*n)++].unicode = unicode;
		}
	}

	if (closedir(d)) perror("closedir"), exit(1);

	qsort(letters, *n, sizeof(struct letter), compare_letters);

	for (i = 0; i < *n; i++) {
		char fullpath[128];
		FILE *f;

		snprintf(fullpath, sizeof fullpath, "%s/%06x.png", path, letters[i].unicode);
		f = fopen(fullpath, "r");
		if (!f) perror(fullpath), exit(1);
		process_file(f, &letters[i]);
		if (fclose(f)) perror("fclose"), exit(1);
	}
}

static void write_letters(struct letter letters[MAX_LETTERS], unsigned n)
{
	unsigned i;
	for (i = 0; i < n; i++) {
		struct letter *l = &letters[i];
		fprintf(output, "\t{ letter_%u, 0x%08x, 0x%08x, %3u, %3u, NULL },\n", l->sequence, l->length, l->unicode, l->x, l->y);
	}
}

static void write_font(char *name, unsigned start, unsigned length)
{
	fprintf(output, "\t{ %u, %u },\n", start, length);
}

int main(void)
{
	output = fopen("font_inc.c", "w");
	if (!output) perror("font_inc.c"), exit(1);
	fprintf(output, "#include \"cfg.h\"\n\n#ifdef G\n\n#include \"links.h\"\n\n");

	load_letters(letters_system, &n_letters_system, "font/system");
	load_letters(letters_normal, &n_letters_normal, "font/normal");
	load_letters(letters_bold, &n_letters_bold, "font/bold");
	load_letters(letters_monospaced, &n_letters_monospaced, "font/monospaced");

	fprintf(output, "struct letter letter_data[%u] = {\n", global_sequence);
	write_letters(letters_system, n_letters_system);
	write_letters(letters_normal, n_letters_normal);
	write_letters(letters_bold, n_letters_bold);
	write_letters(letters_monospaced, n_letters_monospaced);
	fprintf(output, "};\n\n");

	fprintf(output, "struct font font_table[4] = {\n");
	write_font("system", 0, n_letters_system);
	write_font("normal", n_letters_system, n_letters_normal);
	write_font("bold", n_letters_system + n_letters_normal, n_letters_bold);
	write_font("monospaced", n_letters_system + n_letters_normal + n_letters_bold, n_letters_monospaced);
	fprintf(output, "};\n");

	fprintf(output, "\n#endif\n");

	if (fclose(output)) perror("fclose"), exit(1);
}

/* framebuf.c
 * Linux framebuffer code
 * (c) 2002 Petr 'Brain' Kulhavy
 * This file is a part of the Links program, released under GPL.
 */

#include "cfg.h"

#ifdef GRDRV_FB

/*#define USE_FB_ACCEL*/
/*#define USE_FB_ACCEL_FILLRECT*/

/* #define FB_DEBUG */
/* #define SC_DEBUG */

/* note: SIGUSR1 is used by libpthread and is disabled even if no thread
   functions are called --- do not use */

#define SIG_REL	SIGUSR2
#define SIG_ACQ	SIGVTALRM

#if defined(FB_DEBUG) || defined(SC_DEBUG)
#define MESSAGE(a) fprintf(stderr,"%s",a);
#endif

#include "links.h"

#include "bits.h"

#include <gpm.h>

#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>

#include "arrow.inc"

#ifndef MAP_FAILED
#define MAP_FAILED	((void *)-1L)
#endif

#ifdef GPM_HAVE_SMOOTH
#define gpm_smooth GPM_SMOOTH
#else
#define gpm_smooth 0
#endif

#if defined(USE_FB_ACCEL)
#if !defined(FBIO_ACCEL_SUPPORT)
#define FBIO_ACCEL_SUPPORT		0x4630
#define FBIO_ACCEL_SYNC			0x4631
#define FBIO_ACCEL_FILLRECT		0x4632
#define FBIO_ACCEL_FILLRECT_SYNC	0x4633
#define FBIO_ACCEL_COPYAREA		0x4634
#define FBIO_ACCEL_COPYAREA_SYNC	0x4635
#define FB_ACCEL_FILLRECT_SUPPORTED	0x00000001
#define FB_ACCEL_FILLRECT_ACCELERATED	0x00000002
#define FB_ACCEL_COPYAREA_SUPPORTED	0x00000004
#define FB_ACCEL_COPYAREA_ACCELERATED	0x00000008
#define FB_ACCEL_SYNC_NEEDED		0x40000000
/* we don't know if fb_copyarea and fb_fillrect are or aren't defined */
#define fb_copyarea fb_redefined_copyarea
#define fb_fillrect fb_redefined_fillrect
struct fb_copyarea {
	__u32 dx;
	__u32 dy;
	__u32 width;
	__u32 height;
	__u32 sx;
	__u32 sy;
};
struct fb_fillrect {
	__u32 dx;
	__u32 dy;
	__u32 width;
	__u32 height;
	__u32 color;
	__u32 rop;
};
#endif
#ifndef ROP_COPY
#define ROP_COPY	0
#endif
#ifndef ROP_XOR
#define ROP_XOR		1
#endif
#endif

static int TTY = 0;

static int fb_hgpm = -2;
static int fb_hmice = -1;
static unsigned char ps2_mouse_mode;

#define fb_have_mouse	(fb_hgpm >= 0 || fb_hmice >= 0)

static int fb_console;

static struct itrm *fb_kbd;

static struct graphics_device *fb_old_vd;
static struct graphics_device *fb_block_dev;

static int fb_handle = -1;
static unsigned char *fb_mem, *fb_vmem;
static unsigned fb_mem_size;
static unsigned fb_mapped_size;
static int fb_linesize, fb_bits_pp, fb_pixelsize;
static int fb_xsize, fb_ysize;
static int border_left, border_right, border_top, border_bottom;
static int fb_colors, fb_palette_colors;
static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;

static void fb_draw_bitmap(struct graphics_device *dev, struct bitmap *bmp, int x, int y);

static unsigned char *fb_driver_param;
extern struct graphics_driver fb_driver;
static int have_cmap;
static volatile int fb_active;

struct palette {
	unsigned short *red;
	unsigned short *green;
	unsigned short *blue;
};

static struct palette old_palette;
static struct palette global_pal;
static struct vt_mode vt_mode, vt_omode;

/*static struct fb_var_screeninfo oldmode;*/

static volatile int in_gr_operation;

#ifdef USE_FB_ACCEL
static int accel_flags;
static int need_accel_sync;
static inline void accel_sync(void)
{
	int rs;
	if (need_accel_sync) {
		EINTRLOOP(rs, ioctl(fb_handle, FBIO_ACCEL_SYNC));
		need_accel_sync = 0;
	}
}
#else
#define accel_sync()	do { } while (0)
#endif

/* mouse */
static int mouse_x, mouse_y;		/* mouse pointer coordinates */
static long mouse_black, mouse_white;
static int background_x, background_y; /* Where was the mouse background taken from */
static unsigned char *mouse_buffer, *background_buffer, *new_background_buffer;
static struct graphics_device *mouse_graphics_device;
static int global_mouse_hidden;
static int last_mouse_buttons;


#define TEST_MOUSE(xl,xh,yl,yh) if (RECTANGLES_INTERSECT(\
					(xl),(xh),\
					background_x,background_x+arrow_width,\
					(yl),(yh),\
					background_y,background_y+arrow_height)\
					&& !global_mouse_hidden){\
					mouse_hidden=1;\
					hide_mouse();\
				}else mouse_hidden=0;

#define END_MOUSE if (mouse_hidden) show_mouse();

#define END_GR	\
		memory_barrier(&fb_vmem); \
		in_gr_operation--;\
		if (!fb_active && !in_gr_operation) {\
			accel_sync();\
			EINTRLOOP(rs, ioctl(TTY,VT_RELDISP,1));\
		}
#define INC_IN_GR \
		in_gr_operation++; \
		memory_barrier(&fb_vmem);
#define START_GR \
		INC_IN_GR \
		if (!fb_active) { END_GR return; }
#define START_GR_0 \
		INC_IN_GR \
		if (!fb_active) { END_GR return 0; }


#define NUMBER_OF_DEVICES	10

#define TEST_INACTIVITY if (!fb_active||dev!=current_virtual_device) return;

#define TEST_INACTIVITY_0 if (!fb_active||dev!=current_virtual_device) return 0;

#define RECTANGLES_INTERSECT(xl0, xh0, xl1, xh1, yl0, yh0, yl1, yh1) (\
				   (xl0)<(xh1)\
				&& (xl1)<(xh0)\
				&& (yl0)<(yh1)\
				&& (yl1)<(yh0))

/* This assures that x, y, xs, ys, data will be sane according to clipping
 * rectangle. If nothing lies within this rectangle, the current function
 * returns. The data pointer is automatically advanced by this macro to reflect
 * the right position to start with inside the bitmap. */
#define	CLIP_PREFACE \
	int mouse_hidden;\
	int xs, ys;\
	unsigned char *data=bmp->data;\
\
	if (!data)\
		return;\
\
	TEST_INACTIVITY\
	CLIP_DRAW_BITMAP\
	xs = bmp->x;\
	ys = bmp->y;\
	if (x+xs>dev->clip.x2) xs=dev->clip.x2-x;\
	if (y+ys>dev->clip.y2) ys=dev->clip.y2-y;\
	if (dev->clip.x1-x>0){\
		xs-=(dev->clip.x1-x);\
		data+=fb_pixelsize*(dev->clip.x1-x);\
		x=dev->clip.x1;\
	}\
	if (dev->clip.y1-y>0){\
		ys-=(dev->clip.y1-y);\
		data+=bmp->skip*(dev->clip.y1-y);\
		y=dev->clip.y1;\
	}\
	/* xs, ys: how much pixels to paint\
	 * data: where to start painting from\
	 */\
	START_GR\
	TEST_MOUSE(x,x+xs,y,y+ys)


/* fill_area: 5,5,10,10 fills in 25 pixels! */

/* This assures that x1, x2, y1, y2 will be sane according to the
 * clipping rectangle set up by set_clip_area. If empty region
 * results, return from current function occurs. */
#define FILL_CLIP_PREFACE \
	int mouse_hidden;\
	TEST_INACTIVITY\
	CLIP_FILL_AREA\
	START_GR\
	TEST_MOUSE(x1,x2,y1,y2)


#define HLINE_CLIP_PREFACE \
	int mouse_hidden;\
	TEST_INACTIVITY\
	CLIP_DRAW_HLINE\
	START_GR\
	TEST_MOUSE(x1,x2,y,y+1)

#define VLINE_CLIP_PREFACE \
	int mouse_hidden;\
	TEST_INACTIVITY\
	CLIP_DRAW_VLINE\
	START_GR\
	TEST_MOUSE(x,x+1,y1,y2)

#define SCROLL_CLIP_PREFACE \
	int mouse_hidden;\
	TEST_INACTIVITY_0\
	START_GR_0\
	TEST_MOUSE(dev->clip.x1, dev->clip.x2, dev->clip.y1, dev->clip.y2)

#include "fbcommon.inc"

static void redraw_mouse(void);

static void fb_mouse_move(int dx, int dy)
{
	struct links_event ev;
	mouse_x += dx;
	mouse_y += dy;
	ev.ev = EV_MOUSE;
	if (mouse_x >= fb_xsize) mouse_x = fb_xsize - 1;
	if (mouse_y >= fb_ysize) mouse_y = fb_ysize - 1;
	if (mouse_x < 0) mouse_x = 0;
	if (mouse_y < 0) mouse_y = 0;
	ev.x = mouse_x;
	ev.y = mouse_y;
	ev.b = B_MOVE;
	if (((last_mouse_buttons & BM_ACT) == B_DOWN || (last_mouse_buttons & BM_ACT) == B_DRAG) &&
	    !BM_IS_WHEEL(last_mouse_buttons)) {
		ev.b = (last_mouse_buttons & BM_BUTT) | B_DRAG;
	}
	if (!current_virtual_device) return;
	if (current_virtual_device->mouse_handler) current_virtual_device->mouse_handler(current_virtual_device, ev.x, ev.y, (int)ev.b);
	redraw_mouse();
}

static void fb_key_in(struct itrm *p, unsigned char *ev_, int size)
{
	int rs;
	struct links_event *ev = (struct links_event *)(void *)ev_;
	if (size != sizeof(struct links_event)) return;
	if (ev->ev == EV_ABORT) terminate_loop = 1;
	if (ev->ev != EV_KBD) return;
	if (ev->y & KBD_PASTING) goto skip;
	if ((ev->y & (KBD_CTRL | KBD_ALT)) == KBD_ALT && ev->x >= '0' && ev->x <= '9') {
		switch_virtual_device((ev->x - '1' + 10) % 10);
		goto flush_ret;
	}
	if (!ev->y && ev->x == KBD_F5) fb_mouse_move(-3, 0);
	else if (!ev->y && ev->x == KBD_F6) fb_mouse_move(0, 3);
	else if (!ev->y && ev->x == KBD_F7) fb_mouse_move(0, -3);
	else if (!ev->y && ev->x == KBD_F8) fb_mouse_move(3, 0);
	else {
skip:
		if (g_kbd_codepage(&fb_driver) != utf8_table && ev->x >= 128 && ev->x <= 255)
			if ((ev->x = cp2u(ev->x, g_kbd_codepage(&fb_driver))) == -1) return;
		if (current_virtual_device && current_virtual_device->keyboard_handler) current_virtual_device->keyboard_handler(current_virtual_device, ev->x, ev->y);
	}
flush_ret:
	EINTRLOOP(rs, fsync(fb_handle));
}




#define mouse_getscansegment(buf,x,y,w) memcpy_to_fb(buf, fb_vmem + y * fb_linesize + x * fb_pixelsize, w, 1)
#define mouse_drawscansegment(ptr,x,y,w) memcpy_to_fb(fb_vmem + y * fb_linesize + x * fb_pixelsize, ptr, w, 0)

#define do_with_mouse_device(cmd)				\
do {								\
	struct graphics_device *current_virtual_device_backup;	\
								\
	current_virtual_device_backup = current_virtual_device;	\
	current_virtual_device = mouse_graphics_device;		\
	cmd;							\
	current_virtual_device = current_virtual_device_backup;	\
} while (0)

/* Flushes the background_buffer onscreen where it was originally taken from. */
static void place_mouse_background(void)
{
	struct bitmap bmp;

	bmp.x=arrow_width;
	bmp.y=arrow_height;
	bmp.skip=arrow_width*fb_pixelsize;
	bmp.data=background_buffer;

	do_with_mouse_device(fb_draw_bitmap(mouse_graphics_device, &bmp, background_x, background_y));

}

/* Only when the old and new mouse don't interfere. Using it on interfering mouses would
 * cause a flicker.
 */
static void hide_mouse(void)
{
	global_mouse_hidden=1;
	place_mouse_background();
}

/* Gets background from the screen (clipping provided only right and bottom) to the
 * passed buffer.
 */
static void get_mouse_background(unsigned char *buffer_ptr)
{
	int width,height,skip,x,y;

	skip=arrow_width*fb_pixelsize;

	x=mouse_x;
	y=mouse_y;

	width=fb_pixelsize*(arrow_width+x>fb_xsize?fb_xsize-x:arrow_width);
	height=arrow_height+y>fb_ysize?fb_ysize-y:arrow_height;

	accel_sync();

	for (;height;height--){
		mouse_getscansegment(buffer_ptr,x,y,width);
		buffer_ptr+=skip;
		y++;
	}
}

/* Overlays the arrow's image over the mouse_buffer
 * Doesn't draw anything into the screen
 */
static void render_mouse_arrow(void)
{
	int x,y;
	unsigned reg0, reg1;
	unsigned char *mouse_ptr=mouse_buffer;
	const unsigned *arrow_ptr=arrow;

	for (y=arrow_height;y;y--){
		reg0=*arrow_ptr;
		reg1=arrow_ptr[1];
		arrow_ptr+=2;
		for (x=arrow_width;x;)
		{
			unsigned mask=1U<<(--x);

			if (reg0&mask)
				memcpy (mouse_ptr, &mouse_black, fb_pixelsize);
			else if (reg1&mask)
				memcpy (mouse_ptr, &mouse_white, fb_pixelsize);
			mouse_ptr+=fb_pixelsize;
		}
	}
}

static void place_mouse(void)
{
	struct bitmap bmp;

	if (!fb_have_mouse)
		return;

	bmp.x=arrow_width;
	bmp.y=arrow_height;
	bmp.skip=arrow_width*fb_pixelsize;
	bmp.data=mouse_buffer;
	do_with_mouse_device(fb_draw_bitmap(mouse_graphics_device, &bmp, mouse_x, mouse_y));
	global_mouse_hidden=0;
}

/* Only when the old and the new mouse positions do not interfere. Using this routine
 * on interfering positions would cause a flicker.
 */
static void show_mouse(void)
{
	get_mouse_background(background_buffer);
	background_x=mouse_x;
	background_y=mouse_y;
	memcpy(mouse_buffer,background_buffer,fb_pixelsize*arrow_area);
	render_mouse_arrow();
	place_mouse();
}

/* Doesn't draw anything into the screen
 */
static void put_and_clip_background_buffer_over_mouse_buffer(void)
{
	unsigned char *bbufptr=background_buffer, *mbufptr=mouse_buffer;
	int left=background_x-mouse_x;
	int top=background_y-mouse_y;
	int right,bottom;
	int bmpixelsizeL=fb_pixelsize;
	int number_of_bytes;
	int byte_skip;

	right=left+arrow_width;
	bottom=top+arrow_height;

	if (left<0){
		bbufptr-=left*bmpixelsizeL;
		left=0;
	}
	if (right>arrow_width) right=arrow_width;
	if (top<0){
		bbufptr-=top*bmpixelsizeL*arrow_width;
		top=0;
	}
	if (bottom>arrow_height) bottom=arrow_height;
	mbufptr+=bmpixelsizeL*(left+arrow_width*top);
	byte_skip=arrow_width*bmpixelsizeL;
	number_of_bytes=bmpixelsizeL*(right-left);
	for (;top<bottom;top++){
		memcpy(mbufptr,bbufptr,number_of_bytes);
		mbufptr+=byte_skip;
		bbufptr+=byte_skip;
	}
}

/* This draws both the contents of background_buffer and mouse_buffer in a scan
 * way (left-right, top-bottom), so the flicker is reduced.
 */
static inline void place_mouse_composite(void)
{
	int mouse_left=mouse_x;
	int mouse_top=mouse_y;
	int background_left=background_x;
	int background_top=background_y;
	int mouse_right=mouse_left+arrow_width;
	int mouse_bottom=mouse_top+arrow_height;
	int background_right=background_left+arrow_width;
	int background_bottom=background_top+arrow_height;
	int skip=arrow_width*fb_pixelsize;
	int background_length,mouse_length;
	unsigned char *mouse_ptr=mouse_buffer,*background_ptr=background_buffer;
	int yend;

	if (mouse_bottom>fb_ysize) mouse_bottom=fb_ysize;
	if (background_bottom>fb_ysize) background_bottom=fb_ysize;

	accel_sync();

	/* Let's do the top part */
	if (background_top<mouse_top){
		/* Draw the background */
		background_length=background_right>fb_xsize?fb_xsize-background_left
			:arrow_width;
		for (;background_top<mouse_top;background_top++){
			mouse_drawscansegment(background_ptr,background_left
				,background_top,background_length*fb_pixelsize);
			background_ptr+=skip;
		}

	}else if (background_top>mouse_top){
		/* Draw the mouse */
		mouse_length=mouse_right>fb_xsize
			?fb_xsize-mouse_left:arrow_width;
		for (;mouse_top<background_top;mouse_top++){
			mouse_drawscansegment(mouse_ptr,mouse_left,mouse_top,mouse_length*fb_pixelsize);
			mouse_ptr+=skip;
		}
	}

	/* Let's do the middle part */
	yend=mouse_bottom<background_bottom?mouse_bottom:background_bottom;
	if (background_left<mouse_left){
		/* Draw background, mouse */
		mouse_length=mouse_right>fb_xsize?fb_xsize-mouse_left:arrow_width;
		for (;mouse_top<yend;mouse_top++){
			mouse_drawscansegment(background_ptr,background_left,mouse_top
				,(mouse_left-background_left)*fb_pixelsize);
			mouse_drawscansegment(mouse_ptr,mouse_left,mouse_top,mouse_length*fb_pixelsize);
			mouse_ptr+=skip;
			background_ptr+=skip;
		}

	}else{
		int l1, l2, l3;

		/* Draw mouse, background */
		mouse_length=mouse_right>fb_xsize?fb_xsize-mouse_left:arrow_width;
		background_length=background_right-mouse_right;
		if (background_length+mouse_right>fb_xsize)
			background_length=fb_xsize-mouse_right;
		l1=mouse_length*fb_pixelsize;
		l2=(mouse_right-background_left)*fb_pixelsize;
		l3=background_length*fb_pixelsize;
		for (;mouse_top<yend;mouse_top++){
			mouse_drawscansegment(mouse_ptr,mouse_left,mouse_top,l1);
			if (background_length>0)
				mouse_drawscansegment(
					background_ptr + l2,
					mouse_right,mouse_top, l3);
			mouse_ptr+=skip;
			background_ptr+=skip;
		}
	}

	if (background_bottom<mouse_bottom){
		/* Count over bottoms! tops will be invalid! */
		/* Draw mouse */
		mouse_length=mouse_right>fb_xsize?fb_xsize-mouse_left
			:arrow_width;
		for (;background_bottom<mouse_bottom;background_bottom++){
			mouse_drawscansegment(mouse_ptr,mouse_left,background_bottom
				,mouse_length*fb_pixelsize);
			mouse_ptr+=skip;
		}
	}else{
		/* Draw background */
		background_length=background_right>fb_xsize?fb_xsize-background_left
			:arrow_width;
		for (;mouse_bottom<background_bottom;mouse_bottom++){
			mouse_drawscansegment(background_ptr,background_left,mouse_bottom
				,background_length*fb_pixelsize);
			background_ptr+=skip;
		}
	}
}

/* This moves the mouse a sophisticated way when the old and new position of the
 * cursor overlap.
 */
static inline void redraw_mouse_sophisticated(void)
{
	int new_background_x, new_background_y;

	get_mouse_background(mouse_buffer);
	put_and_clip_background_buffer_over_mouse_buffer();
	memcpy(new_background_buffer,mouse_buffer,fb_pixelsize*arrow_area);
	new_background_x=mouse_x;
	new_background_y=mouse_y;
	render_mouse_arrow();
	place_mouse_composite();
	memcpy(background_buffer,new_background_buffer,fb_pixelsize*arrow_area);
	background_x=new_background_x;
	background_y=new_background_y;
}

static void redraw_mouse(void)
{
	int rs;
	START_GR;
	if (mouse_x!=background_x||mouse_y!=background_y){
		if (RECTANGLES_INTERSECT(
			background_x, background_x+arrow_width,
			mouse_x, mouse_x+arrow_width,
			background_y, background_y+arrow_height,
			mouse_y, mouse_y+arrow_height)){
			redraw_mouse_sophisticated();
		}else{
			/* Do a normal hide/show */
			get_mouse_background(mouse_buffer);
			memcpy(new_background_buffer,
				mouse_buffer,arrow_area*fb_pixelsize);
			render_mouse_arrow();
			hide_mouse();
			place_mouse();
			memcpy(background_buffer,new_background_buffer
				,arrow_area*fb_pixelsize);
			background_x=mouse_x;
			background_y=mouse_y;
		}
	}
	END_GR;
}

static void alloc_palette(struct palette *pal)
{
	pal->red = mem_calloc(sizeof(unsigned short) * fb_palette_colors);
	pal->green = mem_calloc(sizeof(unsigned short) * fb_palette_colors);
	pal->blue = mem_calloc(sizeof(unsigned short) * fb_palette_colors);
}

static void free_palette(struct palette *pal)
{
	if (pal->red) mem_free(pal->red);
	if (pal->green) mem_free(pal->green);
	if (pal->blue) mem_free(pal->blue);
}

/* This is an empiric magic that ensures
 * Good white purity
 * Correct rounding and dithering prediction
 * And this is the cabbala:
 * 063 021 063
 * 009 009 021
 * 255 085 255
 * 036 036 084
 */
static void generate_palette(struct palette *pal)
{
	int a;

	alloc_palette(pal);

	switch (fb_colors) {
		case 16:
		case 256:
			for (a = 0; a < fb_palette_colors; a++) {
				unsigned rgb[3];
				int cols = fb_colors;
				if (cols == 256 && (fb_driver.depth & 0x300) == 0x300)
					cols = 216;
				q_palette(cols, a, 65535, rgb);
				pal->red[a] = rgb[0];
				pal->green[a] = rgb[1];
				pal->blue[a] = rgb[2];
			}
			break;
		case 32768:
			for (a = 0; a < fb_palette_colors; a++) {
				/*
				pal->red[a] = ((a >> 10) & 31) * (65535 / 31);
				pal->green[a] = ((a >> 5) & 31) * (65535 / 31);
				pal->blue[a] = (a & 31) * (65535 / 31);
				 */
				pal->red[a] =
				pal->green[a] =
				pal->blue[a] = (((a & 31) * 255) / 31) * 257;
			}
			break;
		case 65536:
			for (a = 0; a < fb_palette_colors; a++){
				 /*
				pal->red[a] = ((a >> 11) & 31) * (65535 / 31);
				pal->green[a] = ((a >> 5) & 63) * (65535 / 63);
				pal->blue[a] = (a & 31) * (65535 / 31);
				 */
				pal->green[a] = (((a & 63) * 255) / 64) * 257;
				pal->red[a] =
				pal->blue[a] = (((a & 31) * 255) / 32) * 257;
			}
			break;
		default:
			for (a = 0; a < fb_palette_colors; a++) {
				pal->red[a] =
				pal->green[a] =
				pal->blue[a] = a * 257;
				/* stuff it in both high and low byte */
			}
	}
}

static void set_palette(struct palette *pal)
{
	struct fb_cmap cmap;
	int i;
	unsigned short *red = pal->red;
	unsigned short *green = pal->green;
	unsigned short *blue = pal->blue;
	__u16 *r, *g, *b, *t;
	int rs;

	if (!red || !green || !blue)
		return;

	r = mem_alloc(fb_palette_colors * sizeof(__u16));
	g = mem_alloc(fb_palette_colors * sizeof(__u16));
	b = mem_alloc(fb_palette_colors * sizeof(__u16));
	t = mem_calloc(fb_palette_colors * sizeof(__u16));

	for (i = 0; i < fb_palette_colors; i++) {
		r[i] = red[i];
		g[i] = green[i];
		b[i] = blue[i];
		/*fprintf(stderr, "%d %d %d\n", r[i], g[i], b[i]);*/
		/*fprintf(stderr, "%5x: %5x\t%5x\t%5x\t%5x\n",i,r[i],g[i],b[i],t[i]);*/

	}

	cmap.start = 0;
	cmap.len = fb_palette_colors;
	cmap.red = r;
	cmap.green = g;
	cmap.blue = b;
	cmap.transp = t;

	EINTRLOOP(rs, ioctl(fb_handle, FBIOPUTCMAP, &cmap));
	if (rs == -1) {
		/*error("Cannot set palette\n")*/;
	}

	mem_free(r);
	mem_free(g);
	mem_free(b);
	mem_free(t);
}

static void get_palette(struct palette *pal)
{
	struct fb_cmap cmap;
	int i;
	__u16 *r, *g, *b, *t;
	int rs;

	pal->red = pal->green = pal->blue = NULL;

	r = mem_alloc(fb_palette_colors * sizeof(__u16));
	g = mem_alloc(fb_palette_colors * sizeof(__u16));
	b = mem_alloc(fb_palette_colors * sizeof(__u16));
	t = mem_alloc(fb_palette_colors * sizeof(__u16));

	cmap.start = 0;
	cmap.len = fb_palette_colors;
	cmap.red = r;
	cmap.green = g;
	cmap.blue = b;
	cmap.transp = t;

	EINTRLOOP(rs, ioctl(fb_handle, FBIOGETCMAP, &cmap));
	if (rs == -1) {
		/*error("Cannot get palette\n")*/;
		goto skip;
	}

	alloc_palette(pal);

	for (i = 0; i < fb_palette_colors; i++) {
		/*printf("%d %d %d\n",r[i],g[i],b[i]);*/
		pal->red[i] = r[i];
		pal->green[i] = g[i];
		pal->blue[i] = b[i];
	}

skip:
	mem_free(r);
	mem_free(g);
	mem_free(b);
	mem_free(t);
}

static void fb_clear_videoram(void)
{
	int rs;
	START_GR
#ifdef USE_FB_ACCEL_FILLRECT
	if (accel_flags & FB_ACCEL_FILLRECT_ACCELERATED) {
		struct fb_fillrect f;
		f.dx = 0;
		f.dy = 0;
		f.width = fb_xsize + border_left + border_right;
		f.height = fb_ysize + border_top + border_bottom;
		f.color = 0;
		f.rop = ROP_COPY;
		EINTRLOOP(rs, ioctl(fb_handle, FBIO_ACCEL_FILLRECT_SYNC, &f));
		if (rs < 0) {
			error("fb_clear_videoram accel failed\n");
			goto no_accel;
		}
	} else
	no_accel:
#endif
	{
		accel_sync();
		memset(fb_mem, 0, (border_top + fb_ysize + border_bottom) * fb_linesize);
		/*{
			int size = (border_top + fb_ysize + border_bottom) * fb_linesize;
			int pos;
			for (pos = 0; pos < size; pos++) {
				fb_mem[pos] = (unsigned char)pos;
			}
		}*/
	}
	END_GR
}

static void fb_switch_signal_rel(void *data)
{
	int rs;
	fb_active = 0;
	if (!in_gr_operation) {
		accel_sync();
		EINTRLOOP(rs, ioctl(TTY, VT_RELDISP, 1));
	}
}

static void fb_switch_signal_acq(void *data)
{
	struct vt_stat st;
	int rs;

	EINTRLOOP(rs, ioctl(TTY, VT_GETSTATE, &st));
	if (rs) return;
	if (st.v_active != fb_console) return;
	INC_IN_GR
	fb_active = 1;
	EINTRLOOP(rs, ioctl(TTY, VT_RELDISP, VT_ACKACQ));
	/*
	 * There is a race condition in Linux NVidia framebuffer driver
	 * It still draws into a framebuffer here, so we have to sleep
	 */
	portable_sleep(10);
	if (have_cmap && current_virtual_device)
		set_palette(&global_pal);
	END_GR
	if (border_left | border_top | border_right | border_bottom) fb_clear_videoram();
	if (current_virtual_device) current_virtual_device->redraw_handler(current_virtual_device, &current_virtual_device->size);
	EINTRLOOP(rs, fsync(fb_handle));
}


static unsigned char *fb_switch_init(void)
{
	int rs;

	INC_IN_GR

	/* If we use threads, the signal handler may execute on a different
	   thread. framebuf.c doesn't handle different-thread signals, so
	   we must switch to synchronous signal handling when using threads */
	install_signal_handler(SIG_REL, fb_switch_signal_rel, NULL,
#ifndef EXEC_IN_THREADS
		1
#else
		0
#endif
		);
	install_signal_handler(SIG_ACQ, fb_switch_signal_acq, NULL, 0);
	EINTRLOOP(rs, ioctl(TTY, VT_GETMODE, &vt_omode));
	if (rs == -1) {
		in_gr_operation--;
		return stracpy(cast_uchar "Could not get VT mode.\n");
	}
	memcpy(&vt_mode, &vt_omode, sizeof(vt_mode));

	vt_mode.mode = VT_PROCESS;
	vt_mode.waitv = 0;
	vt_mode.relsig = SIG_REL;
	vt_mode.acqsig = SIG_ACQ;

	EINTRLOOP(rs, ioctl(TTY, VT_SETMODE, &vt_mode));
	if (rs == -1) {
		in_gr_operation--;
		return stracpy(cast_uchar "Could not set VT mode.\n");
	}

	EINTRLOOP(rs, ioctl(TTY, VT_WAITACTIVE, fb_console));
	fb_active = 1;

	return NULL;
}

static void fb_switch_shutdown(void)
{
	int rs;
	if (in_gr_operation <= 0) internal_error("fb_switch_shutdown: in_gr_operation %d", in_gr_operation);
	if (!fb_active && in_gr_operation == 1) {
		EINTRLOOP(rs, ioctl(TTY, VT_RELDISP, 1));
	}
	EINTRLOOP(rs, ioctl(TTY, VT_SETMODE, &vt_omode));
	install_signal_handler(SIG_REL, (void (*)(void *))NULL, (void*)SIG_REL, 1);
	install_signal_handler(SIG_ACQ, (void (*)(void *))NULL, (void*)SIG_ACQ, 0);
	in_gr_operation--;
}

static void fb_shutdown_palette(void)
{
	if (have_cmap) {
		if (fb_active)
			set_palette(&old_palette);
		free_palette(&old_palette);
		free_palette(&global_pal);
	}
}

static void fb_ctrl_c(void *i_)
{
	kbd_ctrl_c();
}

static void unhandle_fb_mouse(void);

static void fb_gpm_in(void *nic)
{
	struct links_event ev;
	int g;
	Gpm_Event gev;
	int rs;
	again:
	set_handlers(fb_hgpm, (void (*)(void *))NULL, (void (*)(void *))NULL, NULL);
	save_gpm_signals();
	g = Gpm_GetEvent(&gev);
	restore_gpm_signals();
	if (g <= 0) {
		fb_hgpm = -1;
		hide_mouse();
		unhandle_fb_mouse();
		return;
	}
	set_handlers(fb_hgpm, fb_gpm_in, (void (*)(void *))NULL, NULL);
	if (fb_hmice != -1)
		return;
	/*fprintf(stderr, "%x %x %d %d %d %d\n", gev.type, gev.buttons, gev.dx, gev.dy, gev.wdx, gev.wdy);*/
	if (gev.dx || gev.dy) {
		if (!((int)gev.type & gpm_smooth)) {
			mouse_x += gev.dx * 8;
			mouse_y += gev.dy * 8;
		}
#ifdef GPM_HAVE_SMOOTH
		else {
			mouse_x += gev.dx;
			mouse_y += gev.dy;
		}
#endif
	}
	ev.ev = EV_MOUSE;
	if (mouse_x >= fb_xsize) mouse_x = fb_xsize - 1;
	if (mouse_y >= fb_ysize) mouse_y = fb_ysize - 1;
	if (mouse_x < 0) mouse_x = 0;
	if (mouse_y < 0) mouse_y = 0;

	if (!((int)gev.type & gpm_smooth) && (gev.dx || gev.dy)) {
		mouse_x = (mouse_x + 8) / 8 * 8 - 4;
		mouse_y = (mouse_y + 8) / 8 * 8 - 4;
		if (mouse_x >= fb_xsize) mouse_x = fb_xsize - 1;
		if (mouse_y >= fb_ysize) mouse_y = fb_ysize - 1;
		if (mouse_x < 0) mouse_x = 0;
		if (mouse_y < 0) mouse_y = 0;
	}

	ev.x = mouse_x;
	ev.y = mouse_y;
	if (gev.buttons & GPM_B_LEFT) ev.b = B_LEFT;
	else if (gev.buttons & GPM_B_MIDDLE) ev.b = B_MIDDLE;
	else if (gev.buttons & GPM_B_RIGHT) ev.b = B_RIGHT;
#ifdef GPM_B_FOURTH
	else if (gev.buttons & GPM_B_FOURTH) ev.b = B_FOURTH;
#endif
#ifdef GPM_B_UP
	else if (gev.buttons & GPM_B_UP) ev.b = B_FIFTH;
#endif
#ifdef GPM_B_DOWN
	else if (gev.buttons & GPM_B_DOWN) ev.b = B_SIXTH;
#endif
	else ev.b = 0;
	if ((int)gev.type & GPM_DOWN) ev.b |= B_DOWN;
	else if ((int)gev.type & GPM_UP) ev.b |= B_UP;
	else if ((int)gev.type & GPM_DRAG) ev.b |= B_DRAG;
	else ev.b |= B_MOVE;

#ifdef HAVE_WDX_WDY
	if ((ev.b & BM_ACT) == B_DRAG || (ev.b & BM_ACT) == B_MOVE) {
		if (gev.wdy < 0) {
			ev.b &= ~BM_BUTT;
			ev.b |= B_WHEELDOWN;
		} else if (gev.wdy > 0) {
			ev.b &= ~BM_BUTT;
			ev.b |= B_WHEELUP;
		}
#if 0
	/* it doesn't work anyway - the exps2 protocol doesn't support it and evdev support in gpm is buggy */
		else if (gev.wdx < 0) {
			ev.b &= ~BM_BUTT;
			ev.b |= B_WHEELRIGHT;
		} else if (gev.wdx > 0) {
			ev.b &= ~BM_BUTT;
			ev.b |= B_WHEELLEFT;
#endif
	}
#endif

	if (((ev.b & BM_ACT) == B_MOVE && !(ev.b & BM_BUTT)) || (ev.b & BM_ACT) == B_DRAG) {
		if (can_read(fb_hgpm)) goto again;
	}

	last_mouse_buttons = (int)ev.b;
	if (!current_virtual_device) return;
	if (current_virtual_device->mouse_handler) current_virtual_device->mouse_handler(current_virtual_device, ev.x, ev.y, (int)ev.b);
	redraw_mouse();
	EINTRLOOP(rs, fsync(fb_handle));
}

static void fb_ps2_in(void *nic)
{
	unsigned char buffer[4];

	int rs, dx, dy, dw, button;

	int size = ps2_mouse_mode ? 4 : 3;
	if (hard_read(fb_hmice, buffer, size) != size) {
		unhandle_fb_mouse();
		return;
	}

	dx = buffer[1];
	if (buffer[0] & 0x10) dx |= -0x100;
	dy = buffer[2];
	if (buffer[0] & 0x20) dy |= -0x100;
	dy = -dy;

	button = -1;
	if (buffer[0] & 1) button = B_LEFT;
	else if (buffer[0] & 2) button = B_RIGHT;
	else if (buffer[0] & 4) button = B_MIDDLE;
	else if (ps2_mouse_mode == 4) {
		if (buffer[3] & 0x10) button = B_FOURTH;
		else if (buffer[3] & 0x20) button = B_FIFTH;
	}

	dw = 0;
	if (ps2_mouse_mode == 3) {
		dw = buffer[3];
		if (dw & 0x80) dw |= -0x100;
	}
	if (ps2_mouse_mode == 4) {
		dw = buffer[3] & 0x0f;
		if (dw & 0x08) dw |= -0x10;
	}

	mouse_x += dx;
	mouse_y += dy;

	if (mouse_x >= fb_xsize) mouse_x = fb_xsize - 1;
	if (mouse_y >= fb_ysize) mouse_y = fb_ysize - 1;
	if (mouse_x < 0) mouse_x = 0;
	if (mouse_y < 0) mouse_y = 0;

	if ((last_mouse_buttons & BM_ACT) == B_UP || (last_mouse_buttons & BM_ACT) == B_MOVE) {
		if (button == -1)
			button = B_MOVE;
		else
			button |= B_DOWN;
	} else {
		if (button == -1)
			button = (last_mouse_buttons & BM_BUTT) | B_UP;
		else
			button |= B_DRAG;
	}

	if ((button & BM_ACT) == B_DRAG || (button & BM_ACT) == B_MOVE) {
		if (dw > 0) {
			button &= ~BM_BUTT;
			button |= B_WHEELDOWN;
		} else if (dw < 0) {
			button &= ~BM_BUTT;
			button |= B_WHEELUP;
		}
	}

	/*fprintf(stderr, "%02x %d %d %d %d\n", buffer[0], mouse_x, mouse_y, button, last_mouse_buttons);*/

	last_mouse_buttons = button;
	if (!current_virtual_device) return;
	if (!fb_active) button = B_MOVE;
	if (current_virtual_device->mouse_handler) current_virtual_device->mouse_handler(current_virtual_device, mouse_x, mouse_y, button);
	redraw_mouse();
	EINTRLOOP(rs, fsync(fb_handle));
}

static int handle_fb_mouse(void)
{
	Gpm_Connect conn;

	fb_hmice = c_open(cast_uchar "/dev/input/mice", O_RDWR | O_NOCTTY);
	if (fb_hmice != -1) {
		unsigned char c;
		static const unsigned char imps_seq[] = { 0xf3, 200, 0xf3, 100, 0xf3, 80, 0xf2 };
		static const unsigned char imex_seq[] = { 0xf3, 200, 0xf3, 200, 0xf3, 80, 0xf2 };
		if (hard_write(fb_hmice, imps_seq, sizeof(imps_seq)) != sizeof(imps_seq))
			goto gpm_fallback;
		do
			if (hard_read(fb_hmice, &c, 1) != 1)
				goto gpm_fallback;
		while (c != 0xfa);
		if (hard_read(fb_hmice, &ps2_mouse_mode, 1) != 1)
			goto gpm_fallback;
		if (!ps2_mouse_mode)
			goto open_gpm;
		if (ps2_mouse_mode != 3)
			goto gpm_fallback;

		if (hard_write(fb_hmice, imex_seq, sizeof(imex_seq)) != sizeof(imex_seq))
			goto gpm_fallback;
		do
			if (hard_read(fb_hmice, &c, 1) != 1)
				goto gpm_fallback;
		while (c != 0xfa);
		if (hard_read(fb_hmice, &ps2_mouse_mode, 1) != 1)
			goto gpm_fallback;
		if (ps2_mouse_mode == 3)
			goto open_gpm;
		if (ps2_mouse_mode != 4)
			goto gpm_fallback;
	}

open_gpm:
	if (0) {
gpm_fallback:
		close_socket(&fb_hmice);
	}

	fb_hgpm = -2;
	conn.eventMask = (unsigned short)~0U;
	conn.defaultMask = gpm_smooth;
	conn.minMod = 0;
	conn.maxMod = (unsigned short)~0U;
	save_gpm_signals();
	fb_hgpm = Gpm_Open(&conn, 0);
	restore_gpm_signals();
	if (fb_hgpm < 0) {
		fb_hgpm = -2;
		if (fb_hmice >= 0)
			goto done;
		unhandle_fb_mouse();
		return -1;
	}
done:
	if (fb_hgpm >= 0)
		set_handlers(fb_hgpm, fb_gpm_in, (void (*)(void *))NULL, NULL);
	if (fb_hmice != -1)
		set_handlers(fb_hmice, fb_ps2_in, (void (*)(void *))NULL, NULL);
#ifdef SIGTSTP
	install_signal_handler(SIGTSTP, sig_tstp, NULL, 0);
#endif
#ifdef SIGCONT
	install_signal_handler(SIGCONT, sig_cont, NULL, 0);
#endif
#ifdef SIGTTIN
	install_signal_handler(SIGTTIN, sig_tstp, NULL, 0);
#endif

	return 0;
}

static void unhandle_fb_mouse(void)
{
	if (fb_hgpm >= 0) set_handlers(fb_hgpm, (void (*)(void *))NULL, (void (*)(void *))NULL, NULL);
	if (fb_hgpm >= -1) {
		save_gpm_signals();
		Gpm_Close();
		restore_gpm_signals();
	}
	fb_hgpm = -2;

	close_socket(&fb_hmice);
#ifdef SIGTSTP
	install_signal_handler(SIGTSTP, sig_tstp, NULL, 0);
#endif
#ifdef SIGCONT
	install_signal_handler(SIGCONT, sig_cont, NULL, 0);
#endif
#ifdef SIGTTIN
	install_signal_handler(SIGTTIN, sig_tstp, NULL, 0);
#endif
}

static const unsigned char seq_hide_cursor[] = "\033[10000B\033[10000C\033[?25l";
static const unsigned char seq_show_cursor[] = "\033[10000D\033[?25h";

static void fb_print(const unsigned char *str)
{
	int wr;
	EINTRLOOP(wr, (int)write(TTY, str, strlen(cast_const_char str)));
	if (wr <= 0)
		EINTRLOOP(wr, (int)write(1, str, strlen(cast_const_char str)));
}

static void fb_hide_cursor(void)
{
	fb_print(seq_hide_cursor);
}

static void fb_show_cursor(void)
{
	fb_print(seq_show_cursor);
}

static void fb_pan_display(void)
{
	int rs;
	vi.xoffset=0;
	vi.yoffset=0;
	EINTRLOOP(rs, ioctl(fb_handle, FBIOPAN_DISPLAY, &vi));
	/* don't check error */
}

static unsigned char *fb_init_driver(unsigned char *param, unsigned char *ignore)
{
	char *se;
	unsigned char *e;
	struct stat st;
	int rs;
	unsigned long ul;

#if 0
	if (getenv("LINKS_MEMCPY_TEST")) {
#define LEN		256
		unsigned long iteration = 0;
		size_t ps = getpagesize();
		unsigned char val = 0;
		unsigned char data[LEN];
		unsigned char test[LEN];
		unsigned char *map;
		unsigned char *from;
		unsigned start, end, i;
		memset(data, 0, LEN);
		memset(test, 0, LEN);
		map = mmap(NULL, ps * 3, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (map == MAP_FAILED) perror("mmap"), exit(1);
		map += ps;
		if (mprotect(map, ps, PROT_READ | PROT_WRITE)) perror("mprotect"), exit(1);
		fprintf(stderr, "testing memcpy...\n");
		while (1) {
			start = (unsigned)random() % LEN;
			end = (unsigned)random() % LEN;
			if (start > end)
				continue;
			/*if ((end - start) < 7) continue;*/
			if (random() & 1)
				from = map + (unsigned)random() % 8;
			else
				from = map + ps - (end - start) - (unsigned)random() % 8;
			if (1) {
				for (i = start; i < end; i++)
					data[i] = val++;
				memcpy(from, data + start, end - start);
				/*fprintf(stderr, "copy %lu: %p, %p, %x\n", iteration, test + start, from, end - start);*/
				memcpy_to_fb(test + start, from, end - start, 1);
				if (memcmp(test, data, LEN))
					break;
			}
			iteration++;
		}
		internal_error("memcpy is broken (%lu): %u, %u (%u), %p", iteration, start, end, end - start, from);
	}
#undef LEN
#endif

	save_terminal();

	TTY = 0;
	EINTRLOOP(rs, ioctl(TTY, VT_GETMODE, &vt_omode));
	if (rs == -1) {
		TTY = 1;
		EINTRLOOP(rs, ioctl(TTY, VT_GETMODE, &vt_omode));
		if (rs == -1) {
			TTY = 0;
		}
	}

	kbd_set_raw = 1;
	fb_old_vd = NULL;
	fb_driver_param=NULL;
	if(param != NULL)
		fb_driver_param=stracpy(param);

	border_left = border_right = border_top = border_bottom = 0;
	if (!param) param=cast_uchar "";
	if (*param) {
		if (*param < '0' || *param > '9') {
			bad_p:
			e = stracpy(cast_uchar "-mode syntax is left_border[,top_border[,right_border[,bottom_border]]]\n");
			goto fail1;
		}
		ul = strtoul(cast_const_char param, &se, 10);
		param = cast_uchar se;
		if (ul > MAXINT / 10) goto bad_p;
		border_left = (int)ul;
		if (*param == ',') param++;
	} else {
		border_left = 0;
	}
	if (*param) {
		if (*param < '0' || *param > '9') goto bad_p;
		ul = strtoul(cast_const_char param, &se, 10);
		param = cast_uchar se;
		if (ul > MAXINT / 10) goto bad_p;
		border_top = (int)ul;
		if (*param == ',') param++;
	} else {
		border_top = border_left;
	}
	if (*param) {
		if (*param < '0' || *param > '9') goto bad_p;
		ul = strtoul(cast_const_char param, &se, 10);
		param = cast_uchar se;
		if (ul > MAXINT / 10) goto bad_p;
		border_right = (int)ul;
		if (*param == ',') param++;
	} else {
		border_right = border_left;
	}
	if (*param) {
		if (*param < '0' || *param > '9') goto bad_p;
		ul = strtoul(cast_const_char param, &se, 10);
		param = cast_uchar se;
		if (ul > MAXINT / 10) goto bad_p;
		border_bottom = (int)ul;
		if (*param == ',') param++;
	} else {
		border_bottom = border_top;
	}
	if (*param) goto bad_p;

	EINTRLOOP(rs, fstat(TTY, &st));
	if (rs) {
		e = stracpy(cast_uchar "Cannon stat stdin.\n");
		goto fail1;
	}

	fb_console = (int)(st.st_rdev & 0xff);

	fb_hide_cursor();

	if ((e = fb_switch_init())) {
		goto fail2;
	}

	fb_handle = c_open(cast_uchar "/dev/fb0", O_RDWR | O_NOCTTY);
	if (fb_handle == -1) {
		e = stracpy(cast_uchar "Cannot open /dev/fb0.\n");
		goto fail3;
	}

	EINTRLOOP(rs, ioctl(fb_handle, FBIOGET_VSCREENINFO, &vi));
	if (rs == -1) {
		e = stracpy(cast_uchar "Cannot get FB VSCREENINFO.\n");
		goto fail4;
	}

	/*oldmode=vi;*/

	EINTRLOOP(rs, ioctl(fb_handle, FBIOGET_FSCREENINFO, &fi));
	if (rs == -1) {
		e = stracpy(cast_uchar "Cannot get FB FSCREENINFO.\n");
		goto fail4;
	}

#if 0
	{
	unsigned i, j, l;
	fprintf(stderr, "\n");
	for (i = 0; i < sizeof(fi); i += 16) {
		l = sizeof(fi) - i;
		if (l > 16) l = 16;
		fprintf(stderr, "fi(%02x):", i);
		for (j = 0; j < l; j++) fprintf(stderr, "%s %02x", j && !(j & 3) ? " |" : "", ((unsigned char *)&fi)[i + j]);
		fprintf(stderr, "\n");
	}
	fprintf(stderr, "\n");
	for (i = 0; i < sizeof(vi); i += 16) {
		l = sizeof(vi) - i;
		if (l > 16) l = 16;
		fprintf(stderr, "vi(%02x):",i);
		for (j = 0; j < l; j++) fprintf(stderr, "%s %02x", j && !(j & 3) ? " |" : "", ((unsigned char *)&vi)[i + j]);
		fprintf(stderr, "\n");
	}
	}
#endif

	fb_xsize = vi.xres;
	fb_ysize = vi.yres;
	fb_bits_pp = vi.bits_per_pixel;
	if (fb_bits_pp == 16 && vi.green.length == 5) fb_bits_pp = 15;

	if (fb_xsize <= border_left + border_right) border_left = border_right = 0;
	fb_xsize -= border_left + border_right;
	if (fb_ysize <= border_top + border_bottom) border_top = border_bottom = 0;
	fb_ysize -= border_top + border_bottom;

	fb_driver.x=fb_xsize;
	fb_driver.y=fb_ysize;

	switch (fb_bits_pp) {
#if 0
		case 4:
			fb_pixelsize = 1;
			fb_palette_colors = 16;
			break;
#endif

		case 8:
			fb_pixelsize = 1;
			fb_palette_colors = 256;
			break;

		case 15:
		case 16:
			fb_pixelsize = 2;
			fb_palette_colors = 64;
			break;

		case 24:
			fb_palette_colors = 256;
			fb_pixelsize = 3;
			break;

		case 32:
			fb_palette_colors = 256;
			fb_pixelsize = 4;
			fb_bits_pp = 24;
			break;

		default:
			e = stracpy(cast_uchar "Unknown bit depth.\n");
			goto fail4;
	}
	fb_colors = 1 << fb_bits_pp;

	/* we must pan before setting palette */
	fb_pan_display();

	have_cmap = 0;
	if (fi.visual == FB_VISUAL_PSEUDOCOLOR && fb_colors <= 0x1000000) {/* set palette */
		have_cmap = 1;
		fb_palette_colors = fb_colors;
	}
	if (fi.visual == FB_VISUAL_DIRECTCOLOR) { /* set pseudo palette */
		have_cmap = 2;
	}

	fb_linesize = fi.line_length;
	fb_mem_size = fi.smem_len;

	init_virtual_devices(&fb_driver, NUMBER_OF_DEVICES);
	fb_kbd = handle_svgalib_keyboard(fb_key_in);

	/* Mikulas: nechodi to na sparcu */
	if (fb_mem_size < (unsigned)((border_top + fb_ysize + border_bottom) * fb_linesize)) {
		e = stracpy(cast_uchar "Nonlinear mapping of graphics memory not supported.\n");
		goto fail5;
	}

	if (vi.nonstd) {
		e = stracpy(cast_uchar "Non-standard pixel format.\n");
		goto fail5;
	}

	fb_driver.flags |= GD_DONT_USE_SCROLL;
#ifdef USE_FB_ACCEL
	need_accel_sync = 0;
	if (getenv("LINKS_NO_FB_ACCEL")) {
		rs = -1;
	} else {
		EINTRLOOP(rs, ioctl(fb_handle, FBIO_ACCEL_SUPPORT));
	}
	if (rs < 0) {
		accel_flags = 0;
		/*debug("accel not supported");*/
	} else {
		accel_flags = rs;
		/*debug("accel supported, flags %x", accel_flags);*/
	}
	if (fb_bits_pp != 8)
		accel_flags &= ~(FB_ACCEL_FILLRECT_SUPPORTED | FB_ACCEL_FILLRECT_ACCELERATED);
	if (accel_flags & FB_ACCEL_COPYAREA_ACCELERATED)
		fb_driver.flags &= ~GD_DONT_USE_SCROLL;
#endif

	/*
	 * Some framebuffer implementations (for example Mach64) on Sparc64 hate
	 * partial framebuffer mappings.
	 *
	 * For others, we can save virtual memory space by doing a partial mmap.
	 */
	fb_mapped_size = (border_top + fb_ysize + border_bottom) * fb_linesize;
retry1:
	if ((fb_mem = mmap(NULL, fb_mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_handle, 0)) == MAP_FAILED) {
		if (errno == EINTR) goto retry1;
		if (fb_mapped_size != fb_mem_size) {
			fb_mapped_size = fb_mem_size;
			goto retry1;
		}
		e = stracpy(cast_uchar "Cannot mmap graphics memory.\n");
		goto fail5;
	}
	fb_vmem = fb_mem + border_left * fb_pixelsize + border_top * fb_linesize;
	fb_driver.depth = fb_pixelsize & 7;
	fb_driver.depth |= (fb_bits_pp & 31) << 3;
#if 0
	fprintf(stderr, "\nfb: bytes %u, bits %u\n", fb_pixelsize, fb_bits_pp);
	fprintf(stderr, "r: %x %x %x\n", vi.red.offset, vi.red.length, vi.red.msb_right);
	fprintf(stderr, "g: %x %x %x\n", vi.green.offset, vi.green.length, vi.green.msb_right);
	fprintf(stderr, "b: %x %x %x\n", vi.blue.offset, vi.blue.length, vi.blue.msb_right);
	fprintf(stderr, "t: %x %x %x\n", vi.transp.offset, vi.transp.length, vi.transp.msb_right);
#endif
	if (big_endian) {
		/* Big endian */
		if (fb_driver.depth == 130 || fb_driver.depth == 122) fb_driver.depth |= 1 << 8;
		else if (fb_driver.depth == 196) fb_driver.depth |= 1 << 9;
	}
	if (fb_driver.depth == 65 && have_cmap) {
		if (!fb_driver.param->palette_mode)
			fb_driver.depth |= 0x300;
		fb_driver.flags |= GD_SELECT_PALETTE;
	} else {
		fb_driver.flags &= ~GD_SELECT_PALETTE;
	}

	fb_driver.get_color = get_color_fn(fb_driver.depth);
	if (!fb_driver.get_color) {
		e = stracpy(cast_uchar "Unknown bit format.\n");
		goto fail6;
	}

	if (have_cmap) {
		get_palette(&old_palette);
		generate_palette(&global_pal);
		set_palette(&global_pal);
	}

	install_signal_handler(SIGINT, fb_ctrl_c, fb_kbd, 0);

	/* mouse */
	mouse_buffer = mem_alloc(fb_pixelsize * arrow_area);
	background_buffer = mem_alloc(fb_pixelsize * arrow_area);
	new_background_buffer = mem_alloc(fb_pixelsize * arrow_area);
	background_x = mouse_x = fb_xsize >> 1;
	background_y = mouse_y = fb_ysize >> 1;
	mouse_black = fb_driver.get_color(0);
	mouse_white = fb_driver.get_color(0xffffff);
	mouse_graphics_device = fb_driver.init_device();
	virtual_devices[0] = NULL;
	global_mouse_hidden = 1;
	last_mouse_buttons = B_MOVE;
	handle_fb_mouse();
	/* hide cursor */
	if (border_left | border_top | border_right | border_bottom) fb_clear_videoram();

	show_mouse();

	END_GR

#if 0
	if (getenv("LINKS_MEMCPY_BENCH")) {
		unsigned array2[20000];
		unsigned i;
		for (i = 0; i < 327680; i++) {
			/*memcpy_to_fb((unsigned char *)fb_mem, (unsigned char *)array2 + 0, 2048);*/
			/*memcpy_to_fb((unsigned char *)fb_mem + (i & 31), (unsigned char *)array2 + ((i >> 5) & 31), 11000);*/
			/*memcpy_to_fb((unsigned char *)fb_mem + (i & 31), (unsigned char *)array2 + ((i >> 5) & 31), ((i >> 10) & 31) + 1, 1);*/
			memcpy_to_fb((unsigned char *)array2 + ((i >> 5) & 31), (unsigned char *)fb_mem + (i & 31), ((i >> 10) & 8191) + 1, 1);
		}
		exit(1);
	}
#endif

	return NULL;

#if 0
fail7:
	fb_driver.shutdown_device(mouse_graphics_device);
	mem_free(mouse_buffer);
	mem_free(background_buffer);
	mem_free(new_background_buffer);
	fb_shutdown_palette();
#endif
fail6:
	EINTRLOOP(rs, munmap(fb_mem, fb_mapped_size));
fail5:
	svgalib_free_trm(fb_kbd);
	shutdown_virtual_devices();
fail4:
	EINTRLOOP(rs, fsync(fb_handle));
	close_socket(&fb_handle);
	restore_terminal();
fail3:
	fb_switch_shutdown();
fail2:
	fb_show_cursor();
fail1:
	if (fb_driver_param) { mem_free(fb_driver_param); fb_driver_param=NULL; }
	return e;
}

static void fb_shutdown_driver(void)
{
	int rs;
	mem_free(mouse_buffer);
	mem_free(background_buffer);
	mem_free(new_background_buffer);
	fb_driver.shutdown_device(mouse_graphics_device);
	unhandle_fb_mouse();
	INC_IN_GR
	if (fb_active) {
		fb_clear_videoram();
		/*EINTRLOOP(rs, ioctl(fb_handle, FBIOPUT_VSCREENINFO, &oldmode));*/
	}
	fb_shutdown_palette();
	EINTRLOOP(rs, fsync(fb_handle));
	close_socket(&fb_handle);
	EINTRLOOP(rs, munmap(fb_mem, fb_mapped_size));
	shutdown_virtual_devices();
	fb_switch_shutdown();
	svgalib_free_trm(fb_kbd);
	if(fb_driver_param) mem_free(fb_driver_param);
	/* show cursor */
	fb_show_cursor();
	install_signal_handler(SIGINT, (void (*)(void *))NULL, NULL, 0);

	if (in_gr_operation) internal_error("fb_shutdown_driver: in_gr_operation %d", in_gr_operation);

	restore_terminal();
}

static void fb_after_fork(void)
{
	int rs;
	if (fb_handle != -1) {
		EINTRLOOP(rs, close(fb_handle));
		fb_handle = -1;
	}
}

static unsigned char *fb_get_driver_param(void)
{
	return fb_driver_param;
}

static void fb_get_margin(int *left, int *right, int *top, int *bottom)
{
	*left = border_left;
	*right = border_right;
	*top = border_top;
	*bottom = border_bottom;
}

static int fb_set_margin(int left, int right, int top, int bottom)
{
	struct rect_set *rs;
	int i, l;
	struct rect r;

	if (left + right >= fb_xsize + border_left + border_right)
		return -1;
	if (top + bottom >= fb_ysize + border_top + border_bottom)
		return -1;

	hide_mouse();

	rs = init_rect_set();
	add_to_rect_set(&rs, &mouse_graphics_device->size);
	r.x1 = left - border_left;
	r.y1 = top - border_top;
	r.x2 = fb_xsize - (right - border_right);
	r.y2 = fb_ysize - (bottom - border_bottom);
	exclude_rect_from_set(&rs, &r);
	do_with_mouse_device(
	for (i = 0; i < rs->m; i++) if (is_rect_valid(&rs->r[i]))
		drv->fill_area(current_virtual_device, rs->r[i].x1, rs->r[i].y1, rs->r[i].x2, rs->r[i].y2, 0);
	);
	mem_free(rs);

	mouse_x += border_left - left;
	mouse_y += border_top - top;
	fb_xsize = fb_xsize + border_left + border_right - (left + right);
	fb_ysize = fb_ysize + border_top + border_bottom - (top + bottom);
	border_left = left;
	border_right = right;
	border_top = top;
	border_bottom = bottom;

	if (mouse_x >= fb_xsize) mouse_x = fb_xsize - 1;
	if (mouse_x < 0) mouse_x = 0;
	if (mouse_y >= fb_ysize) mouse_y = fb_ysize - 1;
	if (mouse_y < 0) mouse_y = 0;

	fb_vmem = fb_mem + border_left * fb_pixelsize + border_top * fb_linesize;

	mouse_graphics_device->size.x2 = mouse_graphics_device->clip.x2 = fb_xsize;
	mouse_graphics_device->size.y2 = mouse_graphics_device->clip.y2 = fb_ysize;

	show_mouse();

	if (fb_driver_param) mem_free(fb_driver_param);
	fb_driver_param = init_str();
	l = 0;
	add_num_to_str(&fb_driver_param, &l, left);
	add_chr_to_str(&fb_driver_param, &l, ',');
	add_num_to_str(&fb_driver_param, &l, top);
	add_chr_to_str(&fb_driver_param, &l, ',');
	add_num_to_str(&fb_driver_param, &l, right);
	add_chr_to_str(&fb_driver_param, &l, ',');
	add_num_to_str(&fb_driver_param, &l, bottom);

	resize_virtual_devices(fb_xsize, fb_ysize);

	return 0;
}

/* Return value:        0 alloced on heap
 *                      1 alloced in vidram
 *                      2 alloced in X server shm
 */
static int fb_get_empty_bitmap(struct bitmap *dest)
{
	dest->data = NULL;
	if (dest->x && (size_t)dest->x * (size_t)dest->y / (size_t)dest->x != (size_t)dest->y)
		return -1;
	if ((size_t)dest->x * (size_t)dest->y > MAX_SIZE_T / 2 / fb_pixelsize)
		return -1;
	dest->data = mem_alloc_mayfail((size_t)dest->x * (size_t)dest->y * fb_pixelsize);
	if (!dest->data)
		return -1;
	dest->skip = (ssize_t)dest->x * fb_pixelsize;
	dest->flags = 0;
	return 0;
}

static void fb_register_bitmap(struct bitmap *bmp)
{
}

static void fb_unregister_bitmap(struct bitmap *bmp)
{
	if (bmp->data)
		mem_free(bmp->data);
}

static void *fb_prepare_strip(struct bitmap *bmp, int top, int lines)
{
	if (!bmp->data)
		return NULL;
	return ((unsigned char *)bmp->data) + bmp->skip * top;
}


static void fb_commit_strip(struct bitmap *bmp, int top, int lines)
{
}


static void fb_draw_bitmap(struct graphics_device *dev, struct bitmap *bmp, int x, int y)
{
	unsigned char *scr_start;
	int rs;
	size_t copy_len;

	CLIP_PREFACE

	accel_sync();

	scr_start = fb_vmem + y * fb_linesize + x * fb_pixelsize;
	copy_len = xs * fb_pixelsize;
	for (; ys; ys--) {
		memcpy_to_fb_inline(scr_start, data, copy_len, 0);
		data += bmp->skip;
		scr_start += fb_linesize;
	}
	END_MOUSE
	END_GR
}


#ifdef USE_FB_ACCEL_FILLRECT
static inline unsigned fb_accel_color(void *ptr)
{
	return *(unsigned char *)ptr;
}
#endif


static void fb_fill_area(struct graphics_device *dev, int x1, int y1, int x2, int y2, long color)
{
	unsigned char *dest;
	int y;
	int rs;

	FILL_CLIP_PREFACE

#ifdef USE_FB_ACCEL_FILLRECT
	if (accel_flags & FB_ACCEL_FILLRECT_ACCELERATED) {
		struct fb_fillrect f;
		f.dx = x1 + border_left;
		f.dy = y1 + border_top;
		f.width = x2 - x1;
		f.height = y2 - y1;
		f.color = fb_accel_color(&color);
		f.rop = ROP_COPY;
		EINTRLOOP(rs, ioctl(fb_handle, FBIO_ACCEL_FILLRECT, &f));
		if (rs < 0) {
			error("fill_area accel failed\n");
			goto no_accel;
		}
		if (accel_flags & FB_ACCEL_SYNC_NEEDED)
			need_accel_sync = 1;
	} else
		no_accel:
#endif
	{
		accel_sync();
		dest=fb_vmem+y1*fb_linesize+x1*fb_pixelsize;
		for (y=y2-y1;y;y--){
			pixel_set(dest,(x2-x1)*fb_pixelsize,&color);
			dest+=fb_linesize;
		}
	}
	END_MOUSE
	END_GR
}


static void fb_draw_hline(struct graphics_device *dev, int x1, int y, int x2, long color)
{
	unsigned char *dest;
	int rs;

	HLINE_CLIP_PREFACE

#ifdef USE_FB_ACCEL_FILLRECT
	if (accel_flags & FB_ACCEL_FILLRECT_ACCELERATED) {
		struct fb_fillrect f;
		f.dx = x1 + border_left;
		f.dy = y + border_top;
		f.width = x2 - x1;
		f.height = 1;
		f.color = fb_accel_color(&color);
		f.rop = ROP_COPY;
		EINTRLOOP(rs, ioctl(fb_handle, FBIO_ACCEL_FILLRECT, &f));
		if (rs < 0) {
			error("draw_hline accel failed\n");
			goto no_accel;
		}
		if (accel_flags & FB_ACCEL_SYNC_NEEDED)
			need_accel_sync = 1;
	} else
		no_accel:
#endif
	{
		accel_sync();
		dest=fb_vmem+y*fb_linesize+x1*fb_pixelsize;
		pixel_set(dest,(x2-x1)*fb_pixelsize,&color);
	}
	END_MOUSE
	END_GR
}


static void fb_draw_vline(struct graphics_device *dev, int x, int y1, int y2, long color)
{
	unsigned char *dest;
	int y;
	int rs;

	VLINE_CLIP_PREFACE

#ifdef USE_FB_ACCEL_FILLRECT
	if (accel_flags & FB_ACCEL_FILLRECT_ACCELERATED) {
		struct fb_fillrect f;
		f.dx = x + border_left;
		f.dy = y1 + border_top;
		f.width = 1;
		f.height = y2 - y1;
		f.color = fb_accel_color(&color);
		f.rop = ROP_COPY;
		EINTRLOOP(rs, ioctl(fb_handle, FBIO_ACCEL_FILLRECT, &f));
		if (rs < 0) {
			error("draw_vline accel failed\n");
			goto no_accel;
		}
		if (accel_flags & FB_ACCEL_SYNC_NEEDED)
			need_accel_sync = 1;
	} else
		no_accel:
#endif
	{
		accel_sync();
		dest = fb_vmem + y1 * fb_linesize + x * fb_pixelsize;
		y = y2 - y1;
		switch (fb_pixelsize) {
			case 1: {
				unsigned char val = *(unsigned char *)&color;
				do {
					*dest = val;
					dest += fb_linesize;
				} while (--y);
				break;
			}
#ifdef t2c
			case 2: {
				t2c val = *(t2c *)memory_barrier(&color);
				do {
					*(t2c *)dest = val;
					dest += fb_linesize;
				} while (--y);
				break;
			}
#endif
#ifdef t4c
			case 4: {
				t4c val = *(t4c *)memory_barrier(&color);
				do {
					*(t4c *)dest = val;
					dest += fb_linesize;
				} while (--y);
				break;
			}
#endif
			default: {
				do {
					memcpy_to_fb(dest, (unsigned char *)&color, fb_pixelsize, 0);
					dest += fb_linesize;
				} while (--y);
				break;
			}
		}
	}
	END_MOUSE
	END_GR
}


static int fb_scroll(struct graphics_device *dev, struct rect_set **ignore, int scx, int scy)
{
	int rs;

	SCROLL_CLIP_PREFACE

#ifdef USE_FB_ACCEL
	if (accel_flags & FB_ACCEL_COPYAREA_ACCELERATED) {
		struct fb_copyarea f;
		if (scx >= 0) {
			f.sx = dev->clip.x1 + border_left;
			f.width = dev->clip.x2 - dev->clip.x1 - scx;
			f.dx = dev->clip.x1 + scx + border_left;
		} else {
			f.sx = dev->clip.x1 - scx + border_left;
			f.width = dev->clip.x2 - dev->clip.x1 + scx;
			f.dx = dev->clip.x1 + border_left;
		}
		if (scy >= 0) {
			f.sy = dev->clip.y1 + border_top;
			f.height = dev->clip.y2 - dev->clip.y1 - scy;
			f.dy = dev->clip.y1 + scy + border_top;
		} else {
			f.sy = dev->clip.y1 - scy + border_top;
			f.height = dev->clip.y2 - dev->clip.y1 + scy;
			f.dy = dev->clip.y1 + border_top;
		}
		EINTRLOOP(rs, ioctl(fb_handle, FBIO_ACCEL_COPYAREA, &f));
		if (rs < 0) {
			error("hscroll accel failed\n");
			goto no_accel;
		}
		if (accel_flags & FB_ACCEL_SYNC_NEEDED)
			need_accel_sync = 1;
	} else
		no_accel:
#endif
	{
		unsigned char *dest, *src;
		int y, len, dest_off, src_off;

		accel_sync();

		if (scx >= 0) {
			len = (dev->clip.x2 - dev->clip.x1 - scx) * fb_pixelsize;
			dest_off = scx * fb_pixelsize;
			src_off = 0;
		} else {
			len = (dev->clip.x2 - dev->clip.x1 + scx) * fb_pixelsize;
			dest_off = 0;
			src_off = -scx * fb_pixelsize;
		}
		if (scy > 0) {
			/* Down */
			dest = fb_vmem + (dev->clip.y2 - 1) * fb_linesize + dev->clip.x1 * fb_pixelsize;
			src = dest - fb_linesize * scy;
			dest += dest_off;
			src += src_off;
			for (y = dev->clip.y2 - dev->clip.y1 - scy; y; y--) {
				memcpy_to_fb(dest, src, len, 1);
				dest -= fb_linesize;
				src -= fb_linesize;
			}
		} else {
			/* Up */
			dest = fb_vmem + dev->clip.y1 * fb_linesize + dev->clip.x1 * fb_pixelsize;
			src = dest - fb_linesize * scy;
			dest += dest_off;
			src += src_off;
			for (y = dev->clip.y2 - dev->clip.y1 + scy; y; y--) {
				if (scy)
					memcpy_to_fb(dest, src, len, 1);
				else
					memmove_in_fb(dest, src, len);
				dest += fb_linesize;
				src += fb_linesize;
			}
		}
	}
	END_MOUSE
	END_GR
	return 1;
}

static void fb_update_palette(void)
{
	int rs;

	if (!fb_driver.param->palette_mode)
		fb_driver.depth |= 0x300;
	else
		fb_driver.depth &= ~0x300;
	fb_driver.get_color = get_color_fn(fb_driver.depth);
	if (!fb_driver.get_color)
		internal_error("invalid driver depth %d", fb_driver.depth);

	mouse_black = fb_driver.get_color(0);
	mouse_white = fb_driver.get_color(0xffffff);

	free_palette(&global_pal);
	generate_palette(&global_pal);

	START_GR
	set_palette(&global_pal);
	END_GR
}

static int fb_block(struct graphics_device *dev)
{
	int rs;
	if (fb_old_vd) return 1;
	INC_IN_GR
	if (fb_active) accel_sync();
	fb_block_dev = dev;
	unhandle_fb_mouse();
	fb_old_vd = current_virtual_device;
	current_virtual_device=NULL;
	svgalib_block_itrm(fb_kbd);
	if (have_cmap && fb_active) set_palette(&old_palette);
	EINTRLOOP(rs, fsync(fb_handle));
	fb_switch_shutdown();
	fb_clear_videoram();
	fb_show_cursor();
	fb_print(cast_uchar "\n");
	restore_terminal();
	return 0;
}

static int fb_unblock(struct graphics_device *dev)
{
	int rs;
	unsigned char *e;
	if (current_virtual_device) return 0;
	if (dev != fb_block_dev) return -2;
	if (svgalib_unblock_itrm(fb_kbd)) return -1;
	e = fb_switch_init();
	if (e) {
		fatal_exit("fb_switch_init failed: %s", e);
	}
	save_terminal();
	fb_hide_cursor();
	fb_pan_display();
	current_virtual_device = fb_old_vd;
	fb_old_vd = NULL;
	if (have_cmap) set_palette(&global_pal);
	handle_fb_mouse();
	END_GR
	if (border_left | border_top | border_right | border_bottom) fb_clear_videoram();
	if (current_virtual_device) current_virtual_device->redraw_handler(current_virtual_device, &current_virtual_device->size);
	EINTRLOOP(rs, fsync(fb_handle));
	return 0;
}


struct graphics_driver fb_driver = {
	cast_uchar "fb",
	fb_init_driver,
	init_virtual_device,
	shutdown_virtual_device,
	fb_shutdown_driver,
	fb_show_cursor,
	fb_after_fork,
	fb_get_driver_param,
	(unsigned char *(*)(void))NULL,
	fb_get_margin,
	fb_set_margin,
	fb_get_empty_bitmap,
	fb_register_bitmap,
	fb_prepare_strip,
	fb_commit_strip,
	fb_unregister_bitmap,
	fb_draw_bitmap,
	(long (*)(int))NULL,	/* fb_get_color */
	fb_fill_area,
	fb_draw_hline,
	fb_draw_vline,
	fb_scroll,
	(void (*)(struct graphics_device *dev))NULL,			/* set_clip_area */
	(void (*)(struct graphics_device *dev))NULL,			/* flush */
	fb_block,
	fb_unblock,
	fb_update_palette,						/* set_palette */
	NULL,								/* get_real_colors */
	(void (*)(struct graphics_device *, unsigned char *))NULL,	/* set_title */
	(int (*)(unsigned char *, int))NULL,				/* exec */
	(void (*)(struct graphics_device *, unsigned char *))NULL,	/* set_clipboard_text */
	(unsigned char *(*)(void))NULL,					/* get_clipboard_text */
	0,				/* depth (filled in fb_init_driver function) */
	0, 0,				/* size (in X is empty) */
	GD_NEED_CODEPAGE,		/* flags */
	NULL,				/* param */
};

#endif /* GRDRV_FB */

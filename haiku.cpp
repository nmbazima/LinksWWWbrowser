/* haiku.cpp
 * (c) 2007 François Revol
 * This file is a part of the Links program, released under GPL
 */

/*
 * GUI code
 */

/*
 * TODO:
 * - more paste handling ?
 * - more DnD (maybe check if in menu or not and prepend "g" ?)
 * - handle DnD of Net+ bookmarks
 */

#include "cfg.h"

#ifdef GRDRV_HAIKU

extern "C" {
#include "links.h"
}
#undef B_ENTER

#include <app/Application.h>
#include <app/Clipboard.h>
#include <interface/Bitmap.h>
#include <interface/Region.h>
#include <interface/Screen.h>
#include <interface/View.h>
#include <interface/Window.h>
#include <storage/Entry.h>
#include <storage/File.h>
#include <storage/Path.h>
#include <support/Locker.h>
#include <support/String.h>

//#define DBG(l...) fprintf(stderr, l);
#define DBG(l...) {}

/*
#ifdef debug
#undef debug
#endif
#define debug(x)
#define fprintf(x, y)
*/

extern "C" struct graphics_driver haiku_driver;

class LinksApplication : public BApplication {
	public:
	LinksApplication():BApplication("application/x-vnd.links"){}
	virtual bool QuitRequested();
};

class LinksView;

class LinksWindow : public BWindow {
	public:
	LinksWindow(BRect r);
	~LinksWindow();
	virtual void FrameResized(float width, float height);
	virtual bool QuitRequested();
	struct rect update_rect;
	int resized;
	LinksView *view;
};

class LinksView : public BView {
	public:
	LinksView(LinksWindow *w);
	~LinksView();
	virtual void Draw(BRect r);
	virtual void MouseDown(BPoint p);
	virtual void MouseUp(BPoint p);
	virtual void MouseMoved(BPoint p, uint32 transit, const BMessage *dragmsg);
	virtual void KeyDown(const char *s, int32 numBytes);
	virtual void MessageReceived(BMessage *msg);
	LinksWindow *win;
	struct graphics_device *dev;
	void d_flush();
	int flushing;
	int last_x, last_y;
	unsigned last_buttons;
};

#define lv(dev) ((LinksView *)(dev)->driver_data)

#define lock_dev(dev) do { if (!lv(dev)->win->Lock()) return; } while (0)
#define lock_dev0(dev) do { if (!lv(dev)->win->Lock()) return 0; } while (0)
#define unlock_dev(dev) do { lv(dev)->win->Unlock(); } while (0)

static void be_get_size(struct graphics_device *dev);

struct be_event {
	list_entry_1st
	BMessage *msg;
	struct graphics_device *dev;
	list_entry_last
};

static struct list_head be_message_queue;
static BLocker message_queue_lock;

#define detach_and_pipe_message(d) do {					\
	BMessage *current = Looper()->DetachCurrentMessage();		\
	if (current) {							\
		struct be_event *ev = (struct be_event *)malloc(sizeof(struct be_event));\
		message_queue_lock.Lock();				\
		if (ev && d) {						\
			int r;						\
			ev->msg = current;				\
			ev->dev = d;					\
			add_to_list_end(be_message_queue, ev);		\
			EINTRLOOP(r, write(wpipe, " ", 1));		\
		} else {						\
			if (ev) free(ev);				\
			delete current;					\
		}							\
		message_queue_lock.Unlock();				\
	}								\
} while (0)

static LinksApplication *be_links_app;

static int msg_pipe[2];

static thread_id be_app_thread_id;

#define rpipe (msg_pipe[0])
#define wpipe (msg_pipe[1])

#define small_color (sizeof(rgb_color) <= sizeof(long))
#define get_color32(c, rgb) rgb_color \
	c((rgb_color){ \
		static_cast<uint8>((rgb >> 16) & 255), \
		static_cast<uint8>((rgb >> 8) & 255), \
		static_cast<uint8>(rgb & 255), \
		255})

static color_space be_cs_desktop, be_cs_bmp;

static int be_x_size, be_y_size;

static int be_win_x_size, be_win_y_size;
static int be_win_x_pos, be_win_y_pos;

bool LinksApplication::QuitRequested()
{
	int i, n;
	n = CountWindows();
	for (i = 0; i < n; i++) {
		BWindow *win = WindowAt(i);
		if (win) {
			win->PostMessage(B_QUIT_REQUESTED);
		}
	}
	return false;
}

LinksWindow::LinksWindow(BRect r):BWindow(r, "Links", B_DOCUMENT_WINDOW, 0)
{
	DBG("LINKSWINDOW\n");
	update_rect.x1 = 0;
	update_rect.x2 = 0;
	update_rect.y1 = 0;
	update_rect.y2 = 0;
	resized = 0;
	view = NULL;
}

LinksWindow::~LinksWindow()
{
	view = NULL;
	DBG("~LINKSWINDOW\n");
}

void LinksWindow::FrameResized(float width, float height)
{
	message_queue_lock.Lock();
	resized = 1;
	message_queue_lock.Unlock();
}

bool LinksWindow::QuitRequested()
{
	detach_and_pipe_message(view->dev);
	return false;
}

static void do_flush(void *p_dev)
{
	struct graphics_device *dev = (struct graphics_device *)p_dev;
	LinksView *v = lv(dev);
	v->win->Lock();
	v->win->Flush();
	v->win->Unlock();
	v->flushing = 0;
}

LinksView::LinksView(LinksWindow *w):BView(w->Bounds(), "Links", B_FOLLOW_ALL, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_NAVIGABLE)
{
	DBG("LINKSVIEW\n");
	(win = w)->AddChild(this);
	SetViewColor(B_TRANSPARENT_32_BIT);
	MakeFocus();
	w->view = this;
	flushing = 0;
	last_x = last_y = 0;
	last_buttons = 0;
}

LinksView::~LinksView()
{
	win->view = NULL;
	DBG("~LINKSVIEW\n");
}

void LinksView::d_flush()
{
	if (flushing) return;
	register_bottom_half(do_flush, this->dev);
	flushing = 1;
}

static void be_get_event(void *dummy)
{
	struct be_event *ev;
	static char to_read[64];
	int r;
	BMessage *msg;
	LinksView *view;
	LinksWindow *win;
	struct graphics_device *dev;

	EINTRLOOP(r, read(rpipe, to_read, sizeof to_read));

test_another_message:

	message_queue_lock.Lock();
	ev = NULL;
	if (!list_empty(be_message_queue)) {
		ev = list_struct(be_message_queue.next, struct be_event);
		del_from_list(ev);
	}
	message_queue_lock.Unlock();

	if (!ev)
		return;

	msg = ev->msg;
	dev = ev->dev;
	view = lv(dev);
	win = dynamic_cast<LinksWindow *>(view->Window());

	switch (msg->what) {
	case B_QUIT_REQUESTED: {
		dev->keyboard_handler(dev, KBD_CTRL_C, 0);
		break;
	}
	case _UPDATE_: {
		/*DBG("paint: %d %d %d %d\n", rr.x1, rr.x2, rr.y1, rr.y2);*/
		struct rect rr;
		int resized;

		message_queue_lock.Lock();
		rr = win->update_rect;
		win->update_rect.x1 =
		win->update_rect.x2 =
		win->update_rect.y1 =
		win->update_rect.y2 = 0;
		resized = win->resized;
		win->resized = 0;
		message_queue_lock.Unlock();

		if (!resized) {
			if (is_rect_valid(&rr))
				dev->redraw_handler(dev, &rr);
		} else {
			be_get_size(dev);
			dev->resize_handler(dev);
		}
		break;
	}
	case B_MOUSE_DOWN: {
		BPoint where;
		int32 buttons;
		int btn = B_LEFT;
		if (msg->FindInt32("buttons", &buttons) != B_OK)
			return;
		if (msg->FindPoint("where", &where) != B_OK)
			return;
		if (view) view->last_buttons = buttons;
		if (buttons & B_PRIMARY_MOUSE_BUTTON)
			btn = B_LEFT;
		else if (buttons & B_SECONDARY_MOUSE_BUTTON)
			btn = B_RIGHT;
		else if (buttons & B_TERTIARY_MOUSE_BUTTON)
			btn = B_MIDDLE;
		else if (buttons & (B_TERTIARY_MOUSE_BUTTON << 1))
			btn = B_FOURTH;
		else if (buttons & (B_TERTIARY_MOUSE_BUTTON << 2))
			btn = B_FIFTH;
		else if (buttons & (B_TERTIARY_MOUSE_BUTTON << 3))
			btn = B_SIXTH;
		dev->mouse_handler(dev, view->last_x = (int)where.x, view->last_y = (int)where.y, B_DOWN | btn);
		break;
	}
	case B_MOUSE_UP: {
		BPoint where;
		int32 buttons;
		int btn;
		if (msg->FindInt32("buttons", &buttons) != B_OK)
			return;
		if (msg->FindPoint("where", &where) != B_OK)
			return;
		btn = B_LEFT;
		if (view->last_buttons & ~buttons & B_PRIMARY_MOUSE_BUTTON)
			btn = B_LEFT;
		else if (view->last_buttons & ~buttons & B_SECONDARY_MOUSE_BUTTON)
			btn = B_RIGHT;
		else if (view->last_buttons & ~buttons & B_TERTIARY_MOUSE_BUTTON)
			btn = B_MIDDLE;
		else if (view->last_buttons & ~buttons & (B_TERTIARY_MOUSE_BUTTON << 1))
			btn = B_FOURTH;
		else if (view->last_buttons & ~buttons & (B_TERTIARY_MOUSE_BUTTON << 2))
			btn = B_FIFTH;
		else if (view->last_buttons & ~buttons & (B_TERTIARY_MOUSE_BUTTON << 3))
			btn = B_SIXTH;
		view->last_buttons = buttons;
		dev->mouse_handler(dev, view->last_x = (int)where.x, view->last_y = (int)where.y, B_UP | btn);
		break;
	}
	case B_MOUSE_MOVED: {
		BPoint where;
		int32 buttons;
		int btn = B_LEFT;
		if (msg->FindInt32("buttons", &buttons) != B_OK)
			return;
		if (msg->FindPoint("where", &where) != B_OK)
			return;
		if (buttons & B_PRIMARY_MOUSE_BUTTON)
			btn = B_LEFT;
		else if (buttons & B_SECONDARY_MOUSE_BUTTON)
			btn = B_RIGHT;
		else if (buttons & B_TERTIARY_MOUSE_BUTTON)
			btn = B_MIDDLE;
		else if (buttons & (B_TERTIARY_MOUSE_BUTTON << 1))
			btn = B_FOURTH;
		else if (buttons & (B_TERTIARY_MOUSE_BUTTON << 2))
			btn = B_FIFTH;
		else if (buttons & (B_TERTIARY_MOUSE_BUTTON << 3))
			btn = B_SIXTH;
		dev->mouse_handler(dev, view->last_x = (int)where.x, view->last_y = (int)where.y, !buttons ? B_MOVE : B_DRAG | btn);
		break;
	}
	case B_KEY_DOWN: {
		int32 modifiers;
		const char *bytes;
		int c;
		int mods = 0;
		if (msg->FindInt32("modifiers", &modifiers) != B_OK)
			return;
		if (msg->FindString("bytes", &bytes) != B_OK)
			return;
		unsigned char buf[4] = { 0, 0, 0, 0 };
		unsigned char *ss;
		/*fprintf(stderr, "bytes '%s' %x %x, modifiers '%x'\n", bytes, bytes[0], bytes[1], modifiers);*/
		if (modifiers & (B_LEFT_CONTROL_KEY | B_RIGHT_CONTROL_KEY | B_LEFT_COMMAND_KEY | B_RIGHT_COMMAND_KEY)) {
			int32 raw;
			if (msg->FindInt32("raw_char", &raw) != B_OK)
				return;
			buf[0] = (unsigned char)raw;
			ss = buf;
		} else {
			ss = (unsigned char *)bytes;
		}

		GET_UTF_8(ss, c);
		switch (c) {
			case B_BACKSPACE: c = KBD_BS; break;
			case B_ENTER: c = KBD_ENTER; break;
			case B_SPACE: c = ' '; break;
			case B_TAB: c = KBD_TAB; break;
			case B_ESCAPE: c = KBD_ESC; break;
			case B_LEFT_ARROW: c = KBD_LEFT; break;
			case B_RIGHT_ARROW: c = KBD_RIGHT; break;
			case B_UP_ARROW: c = KBD_UP; break;
			case B_DOWN_ARROW: c = KBD_DOWN; break;
			case B_INSERT: c = KBD_INS; break;
			case B_DELETE: c = KBD_DEL; break;
			case B_HOME: c = KBD_HOME; break;
			case B_END: c = KBD_END; break;
			case B_PAGE_UP: c = KBD_PAGE_UP; break;
			case B_PAGE_DOWN: c = KBD_PAGE_DOWN; break;
			case B_FUNCTION_KEY: {
				int32 fn;
				if (msg->FindInt32("key", &fn) != B_OK)
					goto def;
				if (fn >= B_F1_KEY && fn <= B_F12_KEY) {
					c = KBD_F1 - (fn - B_F1_KEY);
					break;
				}
				goto def;
			}
			default:
			def:
				if (c < 32)
					c = 0;
				else modifiers &= ~(B_LEFT_SHIFT_KEY|B_RIGHT_SHIFT_KEY);
				break;
		}
		if (modifiers & (B_LEFT_SHIFT_KEY|B_RIGHT_SHIFT_KEY))
			mods |= KBD_SHIFT;
		if (modifiers & (B_LEFT_CONTROL_KEY|B_RIGHT_CONTROL_KEY))
			mods |= KBD_CTRL;
		if (modifiers & (B_LEFT_COMMAND_KEY|B_RIGHT_COMMAND_KEY))
			mods |= KBD_ALT;
		if (c) dev->keyboard_handler(dev, c, mods);
		break;
	}
	case B_COPY: {
		dev->keyboard_handler(dev, KBD_COPY, 0);
		break;
	}
	case B_CUT: {
		dev->keyboard_handler(dev, KBD_CUT, 0);
		break;
	}
	case B_PASTE: {
		dev->keyboard_handler(dev, KBD_PASTE, 0);
		break;
	}
	case B_MOUSE_WHEEL_CHANGED: {
		float delta_x, delta_y;
		if (msg->FindFloat("be:wheel_delta_x", &delta_x) != B_OK)
			delta_x = 0;
		if (msg->FindFloat("be:wheel_delta_y", &delta_y) != B_OK)
			delta_y = 0;
		if (delta_y) dev->mouse_handler(dev, view->last_x, view->last_y, B_MOVE | (delta_y > 0 ? B_WHEELDOWN : B_WHEELUP));
		if (delta_x) dev->mouse_handler(dev, view->last_x, view->last_y, B_MOVE | (delta_x < 0 ? B_WHEELLEFT : B_WHEELRIGHT));
		break;
	}
	case B_SIMPLE_DATA: {
		entry_ref ref;
		unsigned char *string = NULL;
		const unsigned char *text_plain;
		ssize_t len;
		if (msg->FindRef("refs", &ref) == B_OK) {
			BPath path(&ref);
			if (path.InitCheck() == B_OK) {
				BFile f(path.Path(), B_READ_ONLY);
				BString url;
				if (f.InitCheck() == B_OK && f.ReadAttrString("META:url", &url) >= B_OK) {
					string = stracpy((const unsigned char *)url.String());
				} else {
					unsigned char str[3072];
					int r;

					string = stracpy((const unsigned char *)"file://");

					EINTRLOOP(r, readlink(path.Path(), (char *)str, sizeof(str)));
					if (r < 0 || r >= (int)sizeof(str)) {
						add_to_strn(&string, (unsigned char *)path.Path());
					} else if (str[0] == '/') {
						add_to_strn(&string, str);
					} else {
						unsigned char *rch;
						add_to_strn(&string, (unsigned char *)path.Path());
						rch = (unsigned char *)strrchr((const char *)string, '/');
						if (rch)
							rch[1] = 0;
						add_to_strn(&string, str);
					}
				}
			}
		} else if (msg->FindData("text/plain", B_MIME_TYPE, (const void **)&text_plain, &len) == B_OK) {
			string = memacpy(text_plain, len);
		}
		if (string) {
			dev->extra_handler(dev, EV_EXTRA_OPEN_URL, string);
			mem_free(string);
		}
		break;
	}
	case B_MIME_DATA: {
		const unsigned char *text_plain;
		ssize_t len;
		if (msg->FindData("text/plain", B_MIME_TYPE, (const void **)&text_plain, &len) == B_OK) {
			unsigned char *string = memacpy(text_plain, len);
			dev->extra_handler(dev, EV_EXTRA_OPEN_URL, string);
			mem_free(string);
		}
		break;
	}
	default: {
		msg->PrintToStream();
		break;
	}
	}
	free(ev);
	delete msg;

	goto test_another_message;
}

static void be_get_size(struct graphics_device *dev)
{
	BRect r;
	lock_dev(dev);
	r = lv(dev)->Bounds();
	unlock_dev(dev);
	dev->size.x1 = dev->size.y1 = 0;
	dev->size.x2 = (int)r.Width() + 1;
	dev->size.y2 = (int)r.Height() + 1;
}

void LinksView::Draw(BRect r)
{
	struct rect rr;
	rr.x1 = (int)r.left;
	rr.x2 = (int)r.right + 1;
	rr.y1 = (int)r.top;
	rr.y2 = (int)r.bottom + 1;
	message_queue_lock.Lock();
	if (dev)
		unite_rect(&lv(dev)->win->update_rect, &lv(dev)->win->update_rect, &rr);
	message_queue_lock.Unlock();
	detach_and_pipe_message(dev);
}


void LinksView::MouseDown(BPoint p)
{
	SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
	detach_and_pipe_message(dev);
}

void LinksView::MouseUp(BPoint p)
{
	detach_and_pipe_message(dev);
}

void LinksView::MouseMoved(BPoint p, uint32 transit, const BMessage *dragmsg)
{
	message_queue_lock.Lock();
	if (!list_empty(be_message_queue)) {
		struct be_event *ev = list_struct(be_message_queue.prev, struct be_event);
		if (ev->msg->what == B_MOUSE_MOVED) {
			del_from_list(ev);
			delete ev->msg;
			free(ev);
		}
	}
	message_queue_lock.Unlock();
	detach_and_pipe_message(dev);
}

void LinksView::MessageReceived(BMessage *msg)
{
	switch (msg->what) {
	case B_MOUSE_WHEEL_CHANGED:
	case B_COPY:
	case B_CUT:
	case B_PASTE:
	case B_SIMPLE_DATA:
	case B_MIME_DATA:
		detach_and_pipe_message(dev);
		break;
	default:
		//BView::MessageReceived(msg);
		break;
	}
}

void LinksView::KeyDown(const char *s, int32 numBytes)
{
	detach_and_pipe_message(dev);
}

static int32 be_app_thread(void *p)
{
	be_links_app->Lock();
	be_links_app->Run();
	delete be_links_app;
	return 0;
}

static unsigned char *be_init_driver(unsigned char *param, unsigned char *display)
{
	init_list(be_message_queue);
	be_links_app = new LinksApplication();
	if (!be_links_app) {
		return stracpy((unsigned char *)"Unable to allocate Application object.\n");
	}
	if (c_pipe(msg_pipe)) {
		delete be_links_app;
		return stracpy((unsigned char *)"Could not create pipe.\n");
	}
	set_nonblock(rpipe);
	set_nonblock(wpipe);
	set_handlers(rpipe, be_get_event, NULL, NULL);
	be_app_thread_id = spawn_thread(be_app_thread, "links_app", B_NORMAL_PRIORITY, NULL);
	resume_thread(be_app_thread_id);
	be_links_app->Unlock();
	be_cs_desktop = B_NO_COLOR_SPACE;
	be_x_size = 640;
	be_y_size = 480;
	BScreen d;
	if (d.IsValid()) {
		be_cs_desktop = d.ColorSpace();
		be_x_size = (int)d.Frame().Width() + 1;
		be_y_size = (int)d.Frame().Height() + 1;
	}
	be_win_y_size = be_y_size * 9 / 10;
	be_win_x_size = be_win_y_size;
	/*
	DBG("%d %d\n", be_x_size, be_y_size);
	DBG("%d %d\n", be_win_x_size, be_win_y_size);
	*/
	be_win_y_pos = (be_y_size - be_win_y_size) / 2;
	be_win_x_pos = be_x_size - be_win_x_size - be_win_y_pos;
	/*debug("depth: %d %d %d %d %d", be_cs_desktop, B_RGB15, B_RGB16, B_RGB24, B_RGB32);*/
	be_cs_bmp = be_cs_desktop;
	/* - BeOS doesn't handle BView::DrawBitmap() with RGB24 */
	switch (be_cs_bmp) {
		case B_RGB32:
			haiku_driver.depth = 0xc4;
			break;
		case B_RGB16:
			haiku_driver.depth = 0x82;
			break;
		case B_RGB15:
			haiku_driver.depth = 0x7a;
			break;
		default:
			be_cs_bmp = B_RGB32;
			haiku_driver.depth = 0xc4;
			break;
	}
	return NULL;
}

static void be_shutdown_driver()
{
	status_t ret;
	int r;
	//debug((unsigned char *)"D");
	//debug((unsigned char *)"DD");
	be_links_app->PostMessage(_QUIT_);
	//debug((unsigned char *)"E");
	wait_for_thread(be_app_thread_id, &ret);
	//debug((unsigned char *)"F");
	set_handlers(rpipe, NULL, NULL, NULL);
	EINTRLOOP(r, close(rpipe));
	EINTRLOOP(r, close(wpipe));
}

static struct graphics_device *be_init_device()
{
	LinksView *view;
	LinksWindow *win;
	struct graphics_device *dev = (struct graphics_device *)mem_calloc(sizeof(struct graphics_device));
	//debug((unsigned char *)"1");
	win = new LinksWindow(BRect(be_win_x_pos, be_win_y_pos, be_win_x_pos + be_win_x_size, be_win_y_pos + be_win_y_size));
	be_win_x_pos += 28;
	if (be_win_x_pos + be_win_x_size > be_x_size)
		be_win_x_pos = 5;
	be_win_y_pos += 28;
	if (be_win_y_pos + be_win_y_size > be_y_size)
		be_win_y_pos = 29;
	//debug((unsigned char *)"2");
	if (!win) {
		mem_free(dev);
		return NULL;
	}
	//debug((unsigned char *)"3");
	view = new LinksView(win);
	if (!view) {
		delete win;
		mem_free(dev);
		return NULL;
	}
	view->dev = dev;
	dev->driver_data = view;
	be_get_size(dev);
	memcpy(&dev->clip, &dev->size, sizeof(struct rect));
	//debug((unsigned char *)"4");
	win->Show();
	win->Lock();
	view->MakeFocus();
	win->Unlock();
	//debug((unsigned char *)"5");
	return dev;
}

static void be_shutdown_device(struct graphics_device *dev)
{
	struct be_event *ev;
	struct list_head *lev;
	LinksWindow *win = lv(dev)->win;
	unregister_bottom_half(do_flush, dev);

	message_queue_lock.Lock();
	lv(dev)->dev = NULL;
	foreachback(struct be_event, ev, lev, be_message_queue) {
		if (ev->dev == dev) {
			lev = lev->next;
			del_from_list(ev);
			delete ev->msg;
			free(ev);
		}
	}
	message_queue_lock.Unlock();

	win->PostMessage(_QUIT_);
	mem_free(dev);
}

static unsigned char *be_get_af_unix_name(void)
{
	return cast_uchar "";
}

static void be_set_title(struct graphics_device *dev, unsigned char *title)
{
	LinksWindow *win = lv(dev)->win;
	lock_dev(dev);
	win->SetTitle((const char *)title);
	lv(dev)->d_flush();
	unlock_dev(dev);
}

static int be_get_empty_bitmap(struct bitmap *bmp)
{
	bmp->data = NULL;
	bmp->flags = NULL;
	DBG("bmp\n");
//DBG("bmp (%d, %d) cs %08x\n", bmp->x, bmp->y, be_cs_bmp);
	BRect r(0, 0, bmp->x - 1, bmp->y - 1);
retry:
	BBitmap *b = new BBitmap(r, /*B_RGB32*/be_cs_bmp);
	if (!b) {
		if (out_of_memory(0, NULL, 0))
			goto retry;
DBG("%s: error 1\n", __FUNCTION__);
		return -1;
	}
	if (!b->IsValid()) {
		delete b;
DBG("%s: error 2\n", __FUNCTION__);
		return -1;
	}
	if (b->LockBits() < B_OK) {
		delete b;
DBG("%s: error 3\n", __FUNCTION__);
		return -1;
	}
	bmp->data = b->Bits();
	bmp->skip = b->BytesPerRow();
	bmp->flags = b;
//DBG("bmp: data %p, skip %d, flags %p\n", bmp->data, bmp->skip, bmp->flags);
	return 0;
}

static void be_register_bitmap(struct bitmap *bmp)
{
	BBitmap *b = (BBitmap *)bmp->flags;
	if (b)
		b->UnlockBits();
}

static void *be_prepare_strip(struct bitmap *bmp, int top, int lines)
{
	DBG("preps\n");
	BBitmap *b = (BBitmap *)bmp->flags;
	if (!b)
		return NULL;
	if (b->LockBits() < B_OK)
		return NULL;
	bmp->data = b->Bits();
	bmp->skip = b->BytesPerRow();
	return ((char *)bmp->data) + bmp->skip * top;
}

static void be_commit_strip(struct bitmap *bmp, int top, int lines)
{
	BBitmap *b = (BBitmap *)bmp->flags;
	if (!b)
		return;
	b->UnlockBits();
}

static void be_unregister_bitmap(struct bitmap *bmp)
{
	DBG("unb\n");
	BBitmap *b = (BBitmap *)bmp->flags;
	if (!b)
		return;
	delete b;
}

static void be_draw_bitmap(struct graphics_device *dev, struct bitmap *bmp, int x, int y)
{
	DBG("drawb\n");
	BBitmap *b = (BBitmap *)bmp->flags;
	if (!b)
		return;
	CLIP_DRAW_BITMAP
	lock_dev(dev);
	lv(dev)->DrawBitmap(b, b->Bounds(), BRect(x, y, x + bmp->x - 1, y + bmp->y - 1));
	lv(dev)->d_flush();
	unlock_dev(dev);
}

static long be_get_color(int rgb)
{
	if (small_color) {
		get_color32(c, rgb);
		return *(long *)(void *)&c;
	} else return rgb & 0xffffff;
}

static void *color2void(long *color)
{
	return (void *)color;
}

static void be_fill_area(struct graphics_device *dev, int x1, int y1, int x2, int y2, long color)
{
	DBG("fill\n");
	CLIP_FILL_AREA
	lock_dev(dev);
	if (small_color)
		lv(dev)->SetHighColor(*(rgb_color *)color2void(&color));
	else
		lv(dev)->SetHighColor(get_color32(, color));
	lv(dev)->FillRect(BRect(x1, y1, x2 - 1, y2 - 1));
	lv(dev)->d_flush();
	unlock_dev(dev);
}

static void be_draw_hline(struct graphics_device *dev, int x1, int y, int x2, long color)
{
	DBG("hline\n");
	CLIP_DRAW_HLINE
	lock_dev(dev);
	if (small_color)
		lv(dev)->SetHighColor(*(rgb_color *)color2void(&color));
	else
		lv(dev)->SetHighColor(get_color32(, color));
	lv(dev)->StrokeLine(BPoint(x1, y), BPoint(x2 - 1, y));
	lv(dev)->d_flush();
	unlock_dev(dev);
}

static void be_draw_vline(struct graphics_device *dev, int x, int y1, int y2, long color)
{
	DBG("vline\n");
	CLIP_DRAW_VLINE
	lock_dev(dev);
	if (small_color)
		lv(dev)->SetHighColor(*(rgb_color *)color2void(&color));
	else
		lv(dev)->SetHighColor(get_color32(, color));
	lv(dev)->StrokeLine(BPoint(x, y1), BPoint(x, y2 - 1));
	lv(dev)->d_flush();
	unlock_dev(dev);
}

static void be_scroll_redraws(struct graphics_device *dev, struct rect *r, int scx, int scy)
{
	struct rect *e = &lv(dev)->win->update_rect;
	if (!is_rect_valid(e))
		return;
	if (scx >= 0) {
		if (e->x2 > r->x1 && e->x2 < r->x2) {
			e->x2 += scx;
			if (e->x2 > r->x2) e->x2 = r->x2;
		}
	} else {
		if (e->x1 > r->x1 && e->x1 < r->x2) {
			e->x1 += scx;
			if (e->x1 < r->x1) e->x1 = r->x1;
		}
	}
	if (scy >= 0) {
		if (e->y2 > r->y1 && e->y2 < r->y2) {
			e->y2 += scy;
			if (e->y2 > r->y2) e->y2 = r->y2;
		}
	} else {
		if (e->y1 > r->y1 && e->y1 < r->y2) {
			e->y1 += scy;
			if (e->y1 < r->y1) e->y1 = r->y1;
		}
	}
}

static int be_scroll(struct graphics_device *dev, struct rect_set **ignore, int scx, int scy)
{
	DBG("scroll\n");
	lock_dev0(dev);
	lv(dev)->CopyBits(
		BRect(	dev->clip.x1 - (scx < 0 ? scx : 0),
			dev->clip.y1 - (scy < 0 ? scy : 0),
			dev->clip.x2 - (scx >= 0 ? scx : 0) - 1,
			dev->clip.y2 - (scy >= 0 ? scy : 0) - 1),
		BRect(	dev->clip.x1 + (scx >= 0 ? scx : 0),
			dev->clip.y1 + (scy >= 0 ? scy : 0),
			dev->clip.x2 + (scx < 0 ? scx : 0) - 1,
			dev->clip.y2 + (scy < 0 ? scy : 0) - 1)
	);
	lv(dev)->d_flush();
	be_scroll_redraws(dev, &dev->clip, scx, scy);
	unlock_dev(dev);
	return 1;
}

static void be_set_clip_area(struct graphics_device *dev)
{
	DBG("setc\n");
	lock_dev(dev);
	BRegion clip(BRect(dev->clip.x1, dev->clip.y1, dev->clip.x2 - 1, dev->clip.y2 - 1));
	lv(dev)->ConstrainClippingRegion(&clip);
	unlock_dev(dev);
}

static void be_flush(struct graphics_device *dev)
{
	unregister_bottom_half(do_flush, dev);
	do_flush(dev);
}

static unsigned char *be_get_clipboard_text(void)
{
	unsigned char *ret = NULL;
	if (be_clipboard->Lock()) {
		BMessage *data = be_clipboard->Data();
		if (data) {
			const char *text_plain;
			ssize_t len;
			if (data->FindData("text/plain", B_MIME_TYPE, (const void **)&text_plain, &len) == B_OK) {
				ret = memacpy((unsigned char *)text_plain, len);
			}
		}
		be_clipboard->Unlock();
	}
	return ret;
}

static void be_set_clipboard_text(struct graphics_device *dev, unsigned char *text)
{
	if (be_clipboard->Lock()) {
		be_clipboard->Clear();
		BMessage* data = be_clipboard->Data();
		if (data) {
			data->AddData("text/plain", B_MIME_TYPE, (const char *)text, strlen((const char *)text));
			be_clipboard->Commit();
		}
		be_clipboard->Unlock();
	}
}

struct graphics_driver haiku_driver = {
	(unsigned char *)"haiku",
	be_init_driver,
	be_init_device,
	be_shutdown_device,
	be_shutdown_driver,
	NULL,
	NULL,
	NULL,
	be_get_af_unix_name,
	NULL,
	NULL,
	be_get_empty_bitmap,
	be_register_bitmap,
	be_prepare_strip,
	be_commit_strip,
	be_unregister_bitmap,
	be_draw_bitmap,
	be_get_color,
	be_fill_area,
	be_draw_hline,
	be_draw_vline,
	be_scroll,
	be_set_clip_area,
	be_flush,
	NULL,				/* block */
	NULL,				/* unblock */
	NULL,				/* set_palette */
	NULL,				/* get_real_colors */
	be_set_title,
	x_exec,				/* exec */
	be_set_clipboard_text,		/* set_clipboard_text */
	be_get_clipboard_text,		/* get_clipboard_text */
	0,				/* depth */
	0, 0,				/* size */
	GD_UNICODE_KEYS,		/* flags */
	NULL,				/* param */
};

#endif /* GRDRV_HAIKU */

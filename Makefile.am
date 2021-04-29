# Makefile.am
# (c) 2002 Mikulas Patocka, Karel 'Clock' Kulhavy, Petr 'Brain' Kulhavy,
#          Martin 'PerM' Pergel

man_MANS=links.1

EXTRA_DIST=$(man_MANS) atheos.cpp haiku.cpp BRAILLE_HOWTO KEYS SITES descrip.mms *.opt framebuffer-accelerator.patch gpm-1.20.0-smooth-cursor.patch libpng-1.2.18.patch mailcap.pl rebuild Links_logo.png Unicode/*.cp Unicode/[!C]* certs/[!C]* intl/[!C]* suffix/[!C]* graphics/*.c graphics/arrow.png graphics/gen graphics/font/bold/*.png graphics/font/monospaced/*.png graphics/font/normal/*.png graphics/font/system/*.png doc/links_cal/*.png doc/links_cal/*.html graphics/Makefile graphics/Fontmap graphics/clip.c graphics/links.xpm graphics/makefont graphics/pdf2html graphics/spacer.png pkg-config.m4 *.xpm *.ico *.rc links-haiku.rdef mk-release-os2 mk-release-win mk-dos mk-dos83.cmd links.wis *.nsi
# parser/gen parser/javascr.l parser/javascript.y

bin_PROGRAMS=links

if ATHEOS_GR
links_LDADD=atheos.o
endif
if HAIKU_GR
links_LDADD=haiku.o
all-redirect: all-am $(srcdir)/links-haiku.rdef
	rc -o links-haiku.rsrc $(srcdir)/links-haiku.rdef
	xres -o links$(EXEEXT) links-haiku.rsrc
	mimeset links$(EXEEXT)
	rm -f links-haiku.rsrc
endif
if JAVASCRIPT
links_LDADD=builtin.o context.o ipret.o javascr.o javascript.o md5.o md5hl.o ns.o pomocny.o regexp.o
endif

links_SOURCES=af_unix.c auth.c beos.c bfu.c block.c bookmark.c cache.c charsets.c compress.c connect.c cookies.c data.c default.c dip.c directfb.c dither.c dns.c dos.c drivers.c error.c file.c finger.c fn_impl.c fontconf.c font_inc.c framebuf.c freetype.c ftp.c gif.c grx.c hpux.c html.c html_gr.c html_r.c html_tbl.c http.c https.c img.c imgcache.c jpeg.c jsint.c kbd.c language.c listedit.c lru.c mailto.c main.c memory.c menu.c objreq.c os_dep.c pmshell.c png.c sched.c select.c session.c smb.c string.c suffix.c svg.c svgalib.c terminal.c tiff.c types.c url.c view.c view_gr.c vms.c x.c xbm.c beos.h bits.h cfg.h codepage.h com-defs.h config-vms.h dfb_cur.h hpux.h language.h links.h os_dep.h os_depx.h setup.h arrow.inc certs.inc codepage.inc entity.inc fbcommon.inc language.inc links_ic.inc locase.inc suffix.inc suffix_x.inc uni_7b.inc upcase.inc vpipe.inc
# builtin.c context.c ipret.c javascr.c javascript.c md5.c md5hl.c ns.c pomocny.c regexp.c md5.h ns.h struct.h tree.h typy.h ipret.h javascript.h builtin.h builtin_keys.h

dist-hook:
#remove the symlinka:
	rm $(distdir)/INSTALL
	echo Links $(VERSION) -- How To Install > $(distdir)/INSTALL
	echo Links $(VERSION) -- How To Install | tr -c '\n' '-' >> $(distdir)/INSTALL
	echo >> $(distdir)/INSTALL
	cat $(srcdir)/INSTALL >> $(distdir)/INSTALL

CXXFLAGS=@CXXFLAGS@

atheos.o: $(srcdir)/atheos.cpp
	$(CXX) $(DEFS) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS) -c $(srcdir)/atheos.cpp

haiku.o: $(srcdir)/haiku.cpp
	$(CXX) $(DEFS) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS) -c $(srcdir)/haiku.cpp

datadir = $(prefix)/@DATADIRNAME@
LIBS = @LIBS@


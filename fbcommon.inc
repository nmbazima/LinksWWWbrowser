/* n is in bytes. dest must begin on pixel boundary. If n is not a whole number
 * of pixels, rounding is performed downwards.
 * if bmpixelsize is 1, no alignment is required.
 * if bmpixelsize is 2, dest must be aligned to 2 bytes.
 * if bmpixelsize is 3, no alignment is required.
 * if bmpixelsize is 4, dest must be aligned to 4 bytes.
 * -- The following do not occur, this is only for forward compatibility.
 * if bmpixelsize is 5, no alignment is required.
 * if bmpixelsize is 6, dest must be aligned to 2 bytes.
 * if bmpixelsize is 7, no alignment is required.
 * if bmpixelsize is 8, dest must be aligned to 8 bytes.
 */

static inline void pixel_set(unsigned char *dest, int n, void *pattern)
{
	switch (fb_pixelsize) {
		case 1: {
			memset(dest, *(unsigned char *)pattern, n);
			break;
		}

		case 2: {
#ifdef t2c
			t2c v = *(t2c *)memory_barrier(pattern);
			/* ICC has an optimization bug here */
			icc_volatile int a;

			if ((v & 255) == ((v >> 8) & 255)) {
				memset(dest, v, n);
			} else {
#if defined(t8c) && !(defined(HAVE_GCC_ASSEMBLER) && defined(__i386__))
				t8c vvvv = ((t8c)v << 48) | ((t8c)v << 32) | ((t8c)v << 16) | v;
#elif defined(t4c)
				t4c vv = ((t4c)v << 16) | v;
#endif
				a = n >> 1;
				while (a) {
#if defined(t8c) && !(defined(HAVE_GCC_ASSEMBLER) && defined(__i386__))
					if (!((unsigned long)dest & 7) && a >= 4) {
#if defined(HAVE_GCC_ASSEMBLER) && defined(__x86_64__)
						int tmp;
						__asm__ volatile ("rep stosq" : "=D"(dest), "=c"(tmp) : "D"(dest), "c"(a >> 2), "a"(vvvv) : "memory");
						a &= 3;
#else
						do {
							*((t8c *)dest) = vvvv;
							dest += 8;
							a -= 4;
						} while (a >= 4);
#endif
					} else
#elif defined(t4c)
					if (!((unsigned long)dest & 3) && a >= 2) {
#if defined(HAVE_GCC_ASSEMBLER) && defined(__i386__)
						int tmp;
						__asm__ volatile ("cld; rep stosl" : "=D"(dest), "=c"(tmp) : "D"(dest), "c"(a >> 1), "a"(vv) : "cc", "memory");
						a &= 1;
#else
						do {
							*((t4c *)dest) = vv;
							dest += 4;
							a -= 2;
						} while (a >= 2);
#endif
					} else
#endif
					{
						*((t2c *)dest) = v;
						dest += 2;
						a--;
					}
				}
			}
#else
			unsigned char a, b;
			int i;

			a = *(unsigned char*)pattern;
			b = ((unsigned char*)pattern)[1];
			if (a == b) memset(dest, a, n);
			else for (i = 0; i <= n - 2; i += 2) {
				dest[i] = a;
				dest[i+1] = b;
			}
#endif
			break;
		}

		case 3: {
			unsigned char a, b, c;

			a = *(unsigned char*)pattern;
			b = ((unsigned char*)pattern)[1];
			c = ((unsigned char*)pattern)[2];
			if (a == b && b == c) {
				memset(dest, a, n);
			} else {
#if defined(t4c)
#if defined(t8c_is_efficient)
				t8c t;
				if (!big_endian) {
					t = a | (b << 8) | (c << 16);
				} else {
					t = b | (a << 8) | (c << 16);
				}
				t |= (t << 24) | (t << 48);
#else
				t4c t;
				if (!big_endian) {
					t = a | (b << 8) | (c << 16) | (a << 24);
				} else {
					t = a | (c << 8) | (b << 16) | (a << 24);
				}
#endif
				while (n) {
#if defined(t8c_is_efficient)
					if (!((unsigned long)dest & 7) && n >= 8) {
						do {
							*((t8c *)dest) = t;
							dest += 8;
							n -= 8;
							if (!big_endian) {
								t = (t << 8) | (t >> 16);
							} else {
								t = (t >> 8) | (t << 16);
							}
						} while (n >= 8);
					} else
#else
					if (!((unsigned long)dest & 3) && n >= 4) {
						do {
							*((t4c *)dest) = t;
							dest += 4;
							n -= 4;
							if (!big_endian) {
								t = (t >> 8) | (t << 16);
							} else {
								t = (t << 8) | (t >> 16);
							}
						} while (n >= 4);
					} else
#endif
					{
						if (!big_endian) {
							*dest++ = (unsigned char)t;
							t = (t >> 8) | (t << 16);
						} else {
							*dest++ = (unsigned char)(t
#if defined(t8c_is_efficient)
								>> 8
#endif
								);
							t = (t << 8) | (t >> 16);
						}
						n--;
					}
				}
#else
				int i;
				for (i = 0; i <= n - 3; i += 3) {
					dest[i] = a;
					dest[i + 1] = b;
					dest[i + 2] = c;
				}
#endif
			}
			break;
		}

		case 4: {
			if (((unsigned char *)pattern)[1] == ((unsigned char *)pattern)[2] &&
			    ((unsigned char *)pattern)[1] == ((unsigned char *)pattern)[drv->depth & ~255 ? 3 : 0]) {
				memset(dest, ((unsigned char *)pattern)[1], n);
			} else {
#if defined(HAVE_GCC_ASSEMBLER) && defined(__i386__)
				unsigned v = *(unsigned *)memory_barrier(pattern);
				int tmp;
				__asm__ volatile ("cld; rep stosl" : "=D"(dest), "=c"(tmp) : "D"(dest), "c"(n >> 2), "a"(v) : "cc", "memory");
#elif defined(t4c)
				t4c v=*(t4c *)memory_barrier(pattern);
				/* ICC has an optimization bug here */
				icc_volatile int a;

				{
#ifdef t8c
					t8c vv = ((t8c)v << 32) | v;
#endif
					a = n >> 2;
					while (a) {
#ifdef t8c
						if (!((unsigned long)dest & 7) && a >= 2) {
#if defined(HAVE_GCC_ASSEMBLER) && defined(__x86_64__)
							int tmp;
							__asm__ volatile ("rep stosq" : "=D"(dest), "=c"(tmp) : "D"(dest), "c"(a >> 1), "a"(vv) : "memory");
							a &= 1;
#else
							do {
								*((t8c *)dest) = vv;
								dest += 8;
								a -= 2;
							} while (a >= 2);
#endif
						} else
#endif
						{
							*(t4c *)dest = v;
							dest += 4;
							a--;
						}
					}
				}
#else
				unsigned char a, b, c, d;
				int i;

				a = *(unsigned char*)pattern;
				b = ((unsigned char*)pattern)[1];
				c = ((unsigned char*)pattern)[2];
				d = ((unsigned char*)pattern)[3];
				for (i = 0; i <= n - 4; i += 4) {
					dest[i] = a;
					dest[i + 1] = b;
					dest[i + 2] = c;
					dest[i + 3] = d;
				}
#endif
			}
			break;
		}

#if 0
		default: {
			int a;
			for (a = 0; a < n / fb_pixelsize; a++, dest += fb_pixelsize) memcpy(dest, pattern, fb_pixelsize);
		}
		break;
#endif
	}
}

static inline void memcpy_to_fb_inline(unsigned char *dest, unsigned char *src, size_t len, int from_fb)
{
#ifdef HAVE_GCC_ASSEMBLER
#if defined(__i386__)
#define memcpy_to_fb_implemented
	/* memcpy in glibc 2.17 has half the theoretical througput */
	size_t tmp;
	__asm__ volatile ("		\n\
		cld			\n\
		testw	$3, %%di	\n\
		jz	1f		\n\
		testw	$1, %%di	\n\
		jz	2f		\n"
#ifdef __TINYC__
"		testl	%%ecx, %%ecx	\n\
		je	9f		\n"
#else
"		jecxz	9f		\n"
#endif
"		movsb			\n\
		decl	%%ecx		\n\
		testw	$2, %%di	\n\
		jz	1f		\n\
	2:	movl	%%ecx, %0	\n\
		subl	$2, %%ecx	\n\
		jb	4f		\n\
		movsw			\n\
	1:	movl	%%ecx, %0	\n\
		shrl	$2, %%ecx	\n\
		rep			\n\
		movsl			\n\
		testb	$3, %b0		\n\
		jz	9f		\n\
		testb	$2, %b0		\n\
		jz	3f		\n\
		movsw			\n\
	4:	testb	$1, %b0		\n\
		jz	9f		\n\
	3:	movsb			\n\
	9: " : "=q"(tmp), "=D"(dest), "=S"(src), "=c"(len) : "D"(dest), "S"(src), "c"(len) : "cc", "memory");
	return;
#endif
#if defined(__x86_64__) && !defined(__TINYC__)
#define memcpy_to_fb_implemented
	if (from_fb) {
		size_t l;
		unsigned char buffer[16];
		static int have_sse41 = -1;
		if (have_sse41 < 0) {
			unsigned tmp_eax, ecx;
			__asm__ ("pushq %%rbx; pushq %%rdx; cpuid; popq %%rdx; popq %%rbx" : "=a"(tmp_eax), "=c"(ecx) : "a"(1));
			have_sse41 = (ecx >> 19) & 1;
		}
		if (!have_sse41)
			goto use_memcpy;
		l = -(size_t)src & 0xf;
		if (l) {
			__asm__ volatile ("movntdqa %1, %%xmm0; movdqu %%xmm0, %0" : "=m"(buffer) : "m"(*(src - 0x10 + l)) : "memory");
			memcpy(dest, buffer + 0x10 - l, l < len ? l : len);
			if (l >= len)
				return;
			dest += l;
			src += l;
			len -= l;
		}
		__asm__ volatile ("	\n\
		jmp	2f		\n\
		.p2align 4		\n\
	1:	movntdqa (%1), %%xmm0	\n\
		add	$16, %1		\n\
		movdqu	%%xmm0, (%0)	\n\
		add	$16, %0		\n\
	2:	sub	$16, %2		\n\
		jae	1b		\n\
		" : "=r"(dest), "=r"(src), "=r"(len) : "0"(dest), "1"(src), "2"(len) : "xmm0", "memory", "cc");
		l = len & 0xf;
		if (l) {
			__asm__ volatile ("movntdqa %1, %%xmm0; movdqu %%xmm0, %0" : "=m"(buffer) : "m"(*src) : "memory");
			memcpy(dest, buffer, l);
		}
		return;
	}
	use_memcpy:
#endif
#if defined(__aarch64__) && defined(__ARM_FEATURE_UNALIGNED)
#define memcpy_to_fb_implemented
	/*
	 * This is not faster than the glibc memcpy, but there's some problem
	 * with data corruption in the PCIe controller with unaligned writes
	 */
	unsigned tmp32;
	unsigned long tmp1, tmp2, tmp3, tmp4;
	__asm__ volatile ("			\n\
		tbz	%0, #0, 1f		\n\
		subs	%2, %2, #1		\n\
		b.cc	10f			\n\
		ldrb	%w3, [ %1 ], #1		\n\
		strb	%w3, [ %0 ], #1		\n\
	1:					\n\
		tbz	%0, #1, 2f		\n\
		subs	%2, %2, #2		\n\
		b.cc	9f			\n\
		ldrh	%w3, [ %1 ], #2		\n\
		strh	%w3, [ %0 ], #2		\n\
	2:					\n\
		tbz	%0, #2, 3f		\n\
		subs	%2, %2, #4		\n\
		b.cc	8f			\n\
		ldr	%w3, [ %1 ], #4		\n\
		str	%w3, [ %0 ], #4		\n\
	3:					\n\
		tbz	%0, #3, 4f		\n\
		subs	%2, %2, #8		\n\
		b.cc	7f			\n\
		ldr	%4, [ %1 ], #8		\n\
		str	%4, [ %0 ], #8		\n\
	4:					\n\
		subs	%2, %2, #16		\n\
		b.cc	6f			\n\
						\n\
		tbnz	%2, #4, 55f		\n\
		ldp	%4, %5, [ %1 ], #16	\n\
		stp	%4, %5, [ %0 ], #16	\n\
		subs	%2, %2, #16		\n\
		b.cc	6f			\n\
	55:					\n\
		add	%0, %0, #16		\n\
		add	%1, %1, #16		\n\
		.p2align 3			\n\
	5:					\n\
		ldp	%4, %5, [ %1, #-16 ]	\n\
		ldp	%6, %7, [ %1 ], #32	\n\
		subs	%2, %2, #32		\n\
		stp	%4, %5, [ %0, #-16 ]	\n\
		stp	%6, %7, [ %0 ], #32	\n\
		b.cs	5b			\n\
		sub	%0, %0, #16		\n\
		sub	%1, %1, #16		\n\
	6:					\n\
		tbz	%2, #3, 7f		\n\
		ldr	%4, [ %1 ], #8		\n\
		str	%4, [ %0 ], #8		\n\
	7:					\n\
		tbz	%2, #2, 8f		\n\
		ldr	%w3, [ %1 ], #4		\n\
		str	%w3, [ %0 ], #4		\n\
	8:					\n\
		tbz	%2, #1, 9f		\n\
		ldrh	%w3, [ %1 ], #2		\n\
		strh	%w3, [ %0 ], #2		\n\
	9:					\n\
		tbz	%2, #0, 10f		\n\
		ldrb	%w3, [ %1 ]		\n\
		strb	%w3, [ %0 ]		\n\
	10:					\n\
	" : "=r"(dest), "=r"(src), "=r"(len), "=r"(tmp32), "=r"(tmp1), "=r"(tmp2), "=r"(tmp3), "=r"(tmp4) : "0"(dest), "1"(src), "2"(len) : "cc", "memory");
	return;
#endif
#if defined(__alpha__)
#define memcpy_to_fb_implemented
	/*
	 * The glibc memcpy is very slow because it reads the same value
	 * from the framebuffer multiple times.
	 * There's no point in unrolling the loops because performance is
	 * limited by the bus.
	 */
	unsigned long tmp1, tmp2, tmp3, tmp4;
	__asm__ volatile ("			\n\
		amask	1, $22			\n\
		subq	$31, %0, %3		\n\
		beq	%2, 6f			\n\
		addq	%0, %2, %4		\n\
		and	%3, 7, %3		\n\
		addq	%1, %3, %6		\n\
		cmpult	%2, %3, %5		\n\
		beq	$22, 20f		\n\
		and	%4, 3, $23		\n\
		or	%5, $23, $23		\n\
		beq	$23, 7f			\n\
		ldq_u	$25, -1(%4)		\n\
	7:	beq	%3, 1f			\n\
		blbs	%5, 12f			\n\
		and	%3, 3, $24		\n\
		beq	$24, 8f			\n\
		ldq_u	%4, 0(%0)		\n\
	8:	ldq_u	%5, 0(%1)		\n\
		subq	%2, %3, %2		\n\
		ldq_u	%6, -1(%6)		\n\
		extql	%5, %1, %5		\n\
		extqh	%6, %1, %6		\n\
		addq	%1, %3, %1		\n\
		or	%5, %6, %5		\n\
		beq	$24, 9f			\n\
		insql	%5, %0, %5		\n\
		mskql	%4, %0, %4		\n\
		or	%5, %4, %4		\n\
		stq_u	%4, 0(%0)		\n\
		br	10f			\n\
	9:	stl	%5, 0(%0)		\n\
	10:	addq	%0, %3, %0		\n\
						\n\
	1:	subq	%2, 8, %2		\n\
		blt	%2, 4f			\n\
		and	%1, 7, %4		\n\
		bne	%4, 2f			\n\
		.p2align 3			\n\
	3:	ldq	%3, 0(%1)		\n\
		addq	%1, 8, %1		\n\
		subq	%2, 8, %2		\n\
		stq	%3, 0(%0)		\n\
		addq	%0, 8, %0		\n\
		bge	%2, 3b			\n\
		br	4f			\n\
						\n\
	2:	ldq_u	%5, 0(%1)		\n\
		.p2align 3			\n\
	5:	ldq_u	%4, 8(%1)		\n\
		extql	%5, %1, %5		\n\
		extqh	%4, %1, %6		\n\
		subq	%2, 8, %2		\n\
		or	%5, %6, %5		\n\
		addq	%1, 8, %1		\n\
		stq	%5, 0(%0)		\n\
		mov	%4, %5			\n\
		addq	%0, 8, %0		\n\
		bge	%2, 5b			\n\
						\n\
	4:	and	%2, 7, %2		\n\
	12:	beq	%2, 6f			\n\
		addq	%1, %2, %6		\n\
		ldq_u	$24, 0(%1)		\n\
		ldq_u	%6, -1(%6)		\n\
		extql	$24, %1, $24		\n\
		extqh	%6, %1, %6		\n\
		or	$24, %6, $24		\n\
		beq	$22, 24f		\n\
		beq	$23, 11f		\n\
		mskql	$24, %2, $24		\n\
		addq	%0, %2, %3		\n\
		insql	$24, %0, $24		\n\
		mskql	$25, %0, %6		\n\
		mskqh	$25, %3, $25		\n\
		or	$25, %6, $25		\n\
		or	$25, $24, $24		\n\
		stq_u	$24, 0(%0)		\n\
		br	6f			\n\
						\n\
	11:	stl	$24, 0(%0)		\n\
		br	6f			\n\
						\n\
	20:	beq	%3, 1b			\n\
		addq	%1, %2, %4		\n\
		cmovlbs %5, %4, %6		\n\
		ldq_u	$24, 0(%1)		\n\
		mov	%0, $23			\n\
		ldq_u	$25, -1(%6)		\n\
		extql	$24, %1, $24		\n\
		extqh	$25, %1, $25		\n\
		addq	%0, %3, %0		\n\
		or	$24, $25, $24		\n\
		addq	%1, %3, %1		\n\
		blbc	$23, 21f		\n\
		/*stb	$24, 0($23)*/		\n\
		.long	0x3b170000		\n\
		addq	$23, 1, $23		\n\
		subq	%2, 1, %2		\n\
		srl	$24, 8, $24		\n\
	21:	and	$23, 2, %4		\n\
		beq	%4, 22f			\n\
		subq	%2, 2, %2		\n\
		blt	%2, 26f			\n\
		/*stw	$24, 0($23)*/		\n\
		.long	0x37170000		\n\
		addq	$23, 2, $23		\n\
		srl	$24, 16, $24		\n\
	22:	and	$23, 4, %4		\n\
		beq	%4, 1b			\n\
		subq	%2, 4, %2		\n\
		blt	%2, 25f			\n\
		stl	$24, 0($23)		\n\
	23:	br	1b			\n\
						\n\
	24:	and	%2, 4, %4		\n\
		mov	%0, $23			\n\
		beq	%4, 25f			\n\
		stl	$24, 0(%0)		\n\
		addq	$23, 4, $23		\n\
		srl	$24, 32, $24		\n\
	25:	and	%2, 2, %4		\n\
		beq	%4, 26f			\n\
		/*stw	$24, 0($23)*/		\n\
		.long	0x37170000		\n\
		addq	$23, 2, $23		\n\
		srl	$24, 16, $24		\n\
	26:	blbc	%2, 6f			\n\
		/*stb	$24, 0($23)*/		\n\
		.long	0x3b170000		\n\
						\n\
	6:					\n\
	" : "=r"(dest), "=r"(src), "=r"(len), "=r"(tmp1), "=r"(tmp2), "=r"(tmp3), "=r"(tmp4) : "0"(dest), "1"(src), "2"(len) : "22", "23", "24", "25", "cc", "memory");
	return;
#endif
#endif
	memcpy(dest, src, len);
}

static
#ifdef memcpy_to_fb_implemented
ATTR_NOINLINE
#else
inline
#endif
void memcpy_to_fb(unsigned char *dest, unsigned char *src, size_t len, int from_fb)
{
	memcpy_to_fb_inline(dest, src, len, from_fb);
}

static void memmove_in_fb(unsigned char *dest, unsigned char *src, size_t len)
{
#ifdef memcpy_to_fb_implemented
	static unsigned char *buffer = NULL;
	static size_t buffer_len = 0;
	if (!len)
		return;
	if (len > buffer_len) {
		unsigned char *new_buffer = malloc(len);
		if (!new_buffer)
			goto fallback_to_memmove;
		if (buffer)
			free(buffer);
		buffer = new_buffer;
	}
	memcpy_to_fb(buffer, src, len, 1);
	memcpy_to_fb(dest, buffer, len, 1);
	return;

	fallback_to_memmove:
#endif
	memmove(dest, src, len);
}

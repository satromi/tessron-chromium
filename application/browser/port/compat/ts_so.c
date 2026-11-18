/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	ts_so.c
 *	Linux shared objects in the browser's process (design 14.6.3).
 *
 *	An ELF shared object made for the GNU C library of this processor
 *	(aarch64, or x86-64) is loaded from memory as the dynamic linker of
 *	Linux loads it:
 *
 *	  - its loadable segments are copied into one stretch of addresses
 *	    reserved for the whole of it (ts_map_mem), each at its place
 *	  - the libraries it needs must be ones the GNU C library is made of
 *	    (libc, libm, libdl, libpthread, the dynamic linker), whose calls
 *	    the process has in ts_glibc.c, and the versions of their symbols
 *	    it asks for must be ones known there (a need marked weak may go
 *	    unmet, as with the dynamic linker); anything else is refused
 *	  - its relocations (the processor's kinds) are applied at once (RELR,
 *	    RELA and the PLT's):
 *	    an undefined symbol is bound to the GNU C library's call of that
 *	    name, a weak one the process has not to 0, any other missing one
 *	    refuses the object; its own symbols are its own
 *	  - its thread-local storage (PT_TLS) is reached through TLS
 *	    descriptors whose resolver gives each thread its copy of the
 *	    object's block the first time the thread asks (a thread key holds
 *	    it, and frees it when the thread ends)
 *	  - its code is made executable and read-only, what is read-only
 *	    after relocation (PT_GNU_RELRO) read-only, the rest writable
 *	  - an object that names GCC's unwinder (libgcc_s.so.1) has its
 *	    call frame information made known to the process's unwinder,
 *	    which finds frames only among those registered with it (libgcc's
 *	    __register_frame_info_table_bases): the FDEs its PT_GNU_EH_FRAME
 *	    table lists, until the object is let go
 *	  - its initialising functions (DT_INIT, DT_INIT_ARRAY) are run;
 *	    when the last reference is let go, its ending functions
 *	    (__cxa_atexit's for it, DT_FINI_ARRAY, DT_FINI) are run and its
 *	    memory given back
 *
 *	The object is code the user installed (a CDM): it is run as it is,
 *	with nothing in it changed but what its relocations say.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "ts_so.h"

#if defined(__aarch64__) || defined(__x86_64__)

/* the process's memory (include/ts/umem.h) */
extern int	ts_map_mem( void **p_adr, long size, long align, unsigned prot );
extern int	ts_unm_mem( void *adr, long size );
extern int	ts_ctl_mem( void *adr, long size, unsigned prot );
#define TS_MEM_NONE	0x00
#define TS_MEM_READ	0x01
#define TS_MEM_RW	0x03
#define TS_MEM_EXEC	0x04

#define PAGE		4096UL
#define PG_DOWN(a)	( (uintptr_t)(a) & ~( PAGE - 1 ) )
#define PG_UP(a)	( ( (uintptr_t)(a) + PAGE - 1 ) & ~( PAGE - 1 ) )

/* ---------------------------------------------------------------- ELF64 */

typedef struct {
	unsigned char	e_ident[16];
	uint16_t	e_type, e_machine;
	uint32_t	e_version;
	uint64_t	e_entry, e_phoff, e_shoff;
	uint32_t	e_flags;
	uint16_t	e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Ehdr;

typedef struct {
	uint32_t	p_type, p_flags;
	uint64_t	p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Phdr;

typedef struct {
	int64_t		d_tag;
	uint64_t	d_val;
} Dyn;

typedef struct {
	uint32_t	st_name;
	unsigned char	st_info, st_other;
	uint16_t	st_shndx;
	uint64_t	st_value, st_size;
} Sym;

typedef struct {
	uint64_t	r_offset, r_info;
	int64_t		r_addend;
} Rela;

typedef struct {
	uint16_t	vn_version, vn_cnt;
	uint32_t	vn_file, vn_aux, vn_next;
} Verneed;

typedef struct {
	uint32_t	vna_hash;
	uint16_t	vna_flags, vna_other;
	uint32_t	vna_name, vna_next;
} Vernaux;

#define ET_DYN		3
#define PT_LOAD		1
#define PT_DYNAMIC	2
#define PT_TLS		7
#define PT_GNU_EH_FRAME	0x6474e550
#define PT_GNU_RELRO	0x6474e552
#define PF_X		1
#define PF_W		2

#define DT_NULL		0
#define DT_NEEDED	1
#define DT_PLTRELSZ	2
#define DT_HASH		4
#define DT_STRTAB	5
#define DT_SYMTAB	6
#define DT_RELA		7
#define DT_RELASZ	8
#define DT_RELAENT	9
#define DT_STRSZ	10
#define DT_INIT		12
#define DT_FINI		13
#define DT_REL		17
#define DT_PLTREL	20
#define DT_TEXTREL	22
#define DT_JMPREL	23
#define DT_INIT_ARRAY	25
#define DT_FINI_ARRAY	26
#define DT_INIT_ARRAYSZ	27
#define DT_FINI_ARRAYSZ	28
#define DT_RELRSZ	35
#define DT_RELR		36
#define DT_GNU_HASH	0x6ffffef5
#define DT_VERSYM	0x6ffffff0
#define DT_VERNEED	0x6ffffffe
#define DT_VERNEEDNUM	0x6fffffff
#define VER_FLG_WEAK	0x2

#define STB_LOCAL	0
#define STB_WEAK	2
#define STT_TLS		6
#define STT_GNU_IFUNC	10
#define SHN_UNDEF	0

/*
 * The processor's machine number and its kinds of dynamic relocation
 * (the ELF ABI supplements of aarch64 and of x86-64)
 */
#if defined(__aarch64__)
#define EM_THIS		183		/* EM_AARCH64 */
#define MACHINE		"aarch64"
#define R_NONE		0
#define R_ABS64		257		/* R_AARCH64_ABS64 */
#define R_GLOB_DAT	1025
#define R_JUMP_SLOT	1026
#define R_RELATIVE	1027
#define R_DTPMOD64	1028		/* R_AARCH64_TLS_DTPMOD64 */
#define R_DTPREL64	1029		/* R_AARCH64_TLS_DTPREL64 */
#define R_TPREL64	1030		/* R_AARCH64_TLS_TPREL64 */
#define R_TLSDESC	1031
#define R_IRELATIVE	1032
#else
#define EM_THIS		62		/* EM_X86_64 */
#define MACHINE		"x86-64"
#define R_NONE		0
#define R_ABS64		1		/* R_X86_64_64 */
#define R_GLOB_DAT	6
#define R_JUMP_SLOT	7
#define R_RELATIVE	8
#define R_DTPMOD64	16
#define R_DTPREL64	17		/* R_X86_64_DTPOFF64 */
#define R_TPREL64	18		/* R_X86_64_TPOFF64 */
#define R_TLSDESC	36
#define R_IRELATIVE	37
#endif

/* ---------------------------------------------------------------- the objects */

struct so_tls;

typedef struct so_mod {
	struct so_mod	*next;
	char		name[256];
	int		refs;

	unsigned char	*map;		/* the reserved stretch */
	size_t		mapsz;
	uintptr_t	bias;		/* added to an address of the file */

	const Phdr	*phdr;		/* its program headers, in its memory */
	int		phnum;

	const Dyn	*dyn;
	const Sym	*symtab;
	const char	*strtab;
	size_t		strsz;
	size_t		nsyms;
	const uint16_t	*versym;

	/* thread-local storage: the image of the block, its key */
	int		has_tls;
	size_t		tls_modid;
	const unsigned char *tls_image;
	size_t		tls_filesz, tls_memsz, tls_align;
	pthread_key_t	tls_key;
	struct so_tls	*tls_args;	/* the descriptors' arguments */

	void		(**fini_array)( void );
	size_t		fini_n;
	void		(*fini)( void );

	/* its frames as registered with the process's unwinder: the FDEs, and the unwinder's record */
	const void	**fdes;
	void		*eh_object;
} SoMod;

/* The argument of a TLS descriptor: the object and the offset in its block */
struct so_tls {
	struct so_tls	*next;
	SoMod		*mod;
	uintptr_t	off;
};

static pthread_mutex_t	so_lock = PTHREAD_MUTEX_INITIALIZER;
static SoMod		*so_list;
static size_t		so_next_modid = 1;
static unsigned long long so_adds, so_subs;
static char		so_err[512];
static int		so_err_set;

static void fail( char *err, size_t errsz, const char *fmt, ... )
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(err, errsz, fmt, ap);
	va_end(ap);
	snprintf(so_err, sizeof(so_err), "%s", err);
	so_err_set = 1;
	printf("browser: so: %s\n", err);
}

const char *ts_so_error( void )
{
	if ( !so_err_set ) return NULL;
	so_err_set = 0;
	return so_err;
}

/* ---------------------------------------------------------------- thread-local storage */

static void tls_free( void *block )
{
	free(block);
}

/*
 * The address of a thread's copy of an object's variable: the block made
 * for the thread (its image and zeros) the first time it asks
 */
uintptr_t ts_so_tls_addr( const struct so_tls *arg )
{
	SoMod		*m = arg->mod;
	unsigned char	*block = (unsigned char *)pthread_getspecific(m->tls_key);

	if ( block == NULL ) {
		void *p = NULL;
		size_t align = ( m->tls_align < sizeof(void *) ) ? sizeof(void *) : m->tls_align;
		if ( posix_memalign(&p, align, m->tls_memsz ? m->tls_memsz : 1) != 0 ) {
			printf("browser: so: %s: no memory for a thread's TLS block\n", m->name);
			abort();
		}
		block = (unsigned char *)p;
		memcpy(block, m->tls_image, m->tls_filesz);
		memset(block + m->tls_filesz, 0, m->tls_memsz - m->tls_filesz);
		pthread_setspecific(m->tls_key, block);
	}
	return (uintptr_t)block + arg->off;
}

extern void ts_so_tlsdesc( void );
#if defined(__aarch64__)
/*
 * The resolver of the TLS descriptors: called with x0 the descriptor,
 * whose second word is its argument, it answers in x0 the variable's
 * distance from the thread pointer and keeps every other register as it
 * was (the descriptor's calling convention), the vector registers too.
 */
__asm__(
	"	.text\n"
	"	.p2align 2\n"
	"	.global ts_so_tlsdesc\n"
	"	.type ts_so_tlsdesc, %function\n"
	"ts_so_tlsdesc:\n"
	"	sub sp, sp, #672\n"
	"	stp x29, x30, [sp]\n"
	"	mov x29, sp\n"
	"	stp x1, x2, [sp, #16]\n"
	"	stp x3, x4, [sp, #32]\n"
	"	stp x5, x6, [sp, #48]\n"
	"	stp x7, x8, [sp, #64]\n"
	"	stp x9, x10, [sp, #80]\n"
	"	stp x11, x12, [sp, #96]\n"
	"	stp x13, x14, [sp, #112]\n"
	"	stp x15, x16, [sp, #128]\n"
	"	stp x17, x18, [sp, #144]\n"
	"	stp q0, q1, [sp, #160]\n"
	"	stp q2, q3, [sp, #192]\n"
	"	stp q4, q5, [sp, #224]\n"
	"	stp q6, q7, [sp, #256]\n"
	"	stp q8, q9, [sp, #288]\n"
	"	stp q10, q11, [sp, #320]\n"
	"	stp q12, q13, [sp, #352]\n"
	"	stp q14, q15, [sp, #384]\n"
	"	stp q16, q17, [sp, #416]\n"
	"	stp q18, q19, [sp, #448]\n"
	"	stp q20, q21, [sp, #480]\n"
	"	stp q22, q23, [sp, #512]\n"
	"	stp q24, q25, [sp, #544]\n"
	"	stp q26, q27, [sp, #576]\n"
	"	stp q28, q29, [sp, #608]\n"
	"	stp q30, q31, [sp, #640]\n"
	"	ldr x0, [x0, #8]\n"
	"	bl ts_so_tls_addr\n"
	"	mrs x1, tpidr_el0\n"
	"	sub x0, x0, x1\n"
	"	ldp q0, q1, [sp, #160]\n"
	"	ldp q2, q3, [sp, #192]\n"
	"	ldp q4, q5, [sp, #224]\n"
	"	ldp q6, q7, [sp, #256]\n"
	"	ldp q8, q9, [sp, #288]\n"
	"	ldp q10, q11, [sp, #320]\n"
	"	ldp q12, q13, [sp, #352]\n"
	"	ldp q14, q15, [sp, #384]\n"
	"	ldp q16, q17, [sp, #416]\n"
	"	ldp q18, q19, [sp, #448]\n"
	"	ldp q20, q21, [sp, #480]\n"
	"	ldp q22, q23, [sp, #512]\n"
	"	ldp q24, q25, [sp, #544]\n"
	"	ldp q26, q27, [sp, #576]\n"
	"	ldp q28, q29, [sp, #608]\n"
	"	ldp q30, q31, [sp, #640]\n"
	"	ldp x1, x2, [sp, #16]\n"
	"	ldp x3, x4, [sp, #32]\n"
	"	ldp x5, x6, [sp, #48]\n"
	"	ldp x7, x8, [sp, #64]\n"
	"	ldp x9, x10, [sp, #80]\n"
	"	ldp x11, x12, [sp, #96]\n"
	"	ldp x13, x14, [sp, #112]\n"
	"	ldp x15, x16, [sp, #128]\n"
	"	ldp x17, x18, [sp, #144]\n"
	"	ldp x29, x30, [sp]\n"
	"	add sp, sp, #672\n"
	"	ret\n"
	"	.size ts_so_tlsdesc, . - ts_so_tlsdesc\n"
);
#else
/*
 * The resolver of the TLS descriptors (x86-64's GNU2 dialect): called
 * with %rax the descriptor, whose second word is its argument, it
 * answers in %rax the variable's distance from the thread pointer (%fs:0,
 * the control block's word with its own address) and keeps every other
 * register as it was. The registers a C call may change are saved, the
 * x87 and SSE ones with FXSAVE; the C code it calls is made without AVX
 * (x86-64-v2), so the upper halves of the YMM registers stay as they are.
 */
__asm__(
	"	.text\n"
	"	.p2align 4\n"
	"	.global ts_so_tlsdesc\n"
	"	.type ts_so_tlsdesc, @function\n"
	"ts_so_tlsdesc:\n"
	"	pushq %rbp\n"
	"	movq %rsp, %rbp\n"
	"	pushq %rdi\n"
	"	pushq %rsi\n"
	"	pushq %rdx\n"
	"	pushq %rcx\n"
	"	pushq %r8\n"
	"	pushq %r9\n"
	"	pushq %r10\n"
	"	pushq %r11\n"
	"	subq $512, %rsp\n"
	"	andq $-16, %rsp\n"
	"	fxsave64 (%rsp)\n"
	"	movq 8(%rax), %rdi\n"
	"	call ts_so_tls_addr\n"
	"	subq %fs:0, %rax\n"
	"	fxrstor64 (%rsp)\n"
	"	leaq -64(%rbp), %rsp\n"
	"	popq %r11\n"
	"	popq %r10\n"
	"	popq %r9\n"
	"	popq %r8\n"
	"	popq %rcx\n"
	"	popq %rdx\n"
	"	popq %rsi\n"
	"	popq %rdi\n"
	"	popq %rbp\n"
	"	ret\n"
	"	.size ts_so_tlsdesc, . - ts_so_tlsdesc\n"
);
#endif

/* __tls_get_addr's argument: the module's number and the offset */
typedef struct {
	uintptr_t	modid, off;
} TlsIndex;

/* The general dynamic model's lookup, for objects that ask through __tls_get_addr */
void *ts_so_tls_get_addr( const TlsIndex *ti )
{
	SoMod	*m;
	struct so_tls arg;

	pthread_mutex_lock(&so_lock);
	for ( m = so_list; m != NULL && m->tls_modid != ti->modid; m = m->next ) ;
	pthread_mutex_unlock(&so_lock);
	if ( m == NULL || !m->has_tls ) {
		printf("browser: so: TLS of an unknown module %lu asked for\n", (unsigned long)ti->modid);
		abort();
	}
	arg.mod = m;
	arg.off = ti->off;
	return (void *)ts_so_tls_addr(&arg);
}

/* ---------------------------------------------------------------- symbols */

/*
 * A string of the object's string table. The dynamic linker reads one
 * wherever the offset puts it, DT_STRSZ or not: one past the table's
 * size is taken too, as long as it lies in the object and ends there
 */
static const char *str_at( const SoMod *m, size_t off )
{
	const char	*s, *end = (const char *)m->map + m->mapsz;

	if ( off < m->strsz ) return m->strtab + off;
	s = m->strtab + off;
	if ( s < m->strtab || s < (const char *)m->map || s >= end ) return "";
	if ( memchr(s, 0, (size_t)( end - s )) == NULL ) return "";
	return s;
}

/* The number of symbols, from DT_HASH's chain count or DT_GNU_HASH's buckets and chains */
static size_t count_syms( const SoMod *m, const uint32_t *hash, const uint32_t *gnu )
{
	if ( hash != NULL ) return hash[1];
	if ( gnu != NULL ) {
		uint32_t nbuckets = gnu[0], symoffset = gnu[1], bloom = gnu[2];
		const uint32_t *buckets = gnu + 4 + bloom * 2;	/* the bloom filter's words are 64-bit */
		const uint32_t *chain = buckets + nbuckets;
		uint32_t last = 0, i;
		for ( i = 0; i < nbuckets; i++ ) {
			if ( buckets[i] > last ) last = buckets[i];
		}
		if ( last < symoffset ) return symoffset;
		while ( ( chain[last - symoffset] & 1 ) == 0 ) last++;
		return last + 1;
	}
	(void)m;
	return 0;
}

/* The name of the version symbol 'i' asks for, NULL when none */
static const char *version_of( const SoMod *m, size_t i )
{
	const Dyn	*d;
	uintptr_t	vn = 0;
	size_t		vnnum = 0;
	uint16_t	want;

	if ( m->versym == NULL ) return NULL;
	want = m->versym[i] & 0x7fff;
	if ( want < 2 ) return NULL;
	for ( d = m->dyn; d->d_tag != DT_NULL; d++ ) {
		if ( d->d_tag == DT_VERNEED ) vn = m->bias + d->d_val;
		if ( d->d_tag == DT_VERNEEDNUM ) vnnum = d->d_val;
	}
	while ( vn != 0 && vnnum-- > 0 ) {
		const Verneed *v = (const Verneed *)vn;
		uintptr_t a = vn + v->vn_aux;
		for ( uint16_t k = 0; k < v->vn_cnt; k++ ) {
			const Vernaux *x = (const Vernaux *)a;
			if ( x->vna_other == want ) return str_at(m, x->vna_name);
			if ( x->vna_next == 0 ) break;
			a += x->vna_next;
		}
		if ( v->vn_next == 0 ) break;
		vn += v->vn_next;
	}
	return NULL;
}

/* A symbol the object defines and exports */
static void *own_sym( const SoMod *m, const char *name )
{
	size_t i;

	for ( i = 1; i < m->nsyms; i++ ) {
		const Sym *s = &m->symtab[i];
		int bind = s->st_info >> 4, type = s->st_info & 0xf;
		if ( s->st_shndx == SHN_UNDEF || bind == STB_LOCAL || type == STT_TLS ) continue;
		if ( ( s->st_other & 3 ) == 1 || ( s->st_other & 3 ) == 2 ) continue;	/* internal, hidden */
		if ( strcmp(str_at(m, s->st_name), name) == 0 ) return (void *)( m->bias + s->st_value );
	}
	return NULL;
}

static unsigned long hwcap_of( void )
{
	extern unsigned long getauxval( unsigned long type );
	return getauxval(16);	/* AT_HWCAP */
}

/*
 * What relocation 'r' of symbol 'si' is bound to; *missing set when the
 * symbol is neither the object's nor the process's and is not weak
 */
static uintptr_t bind_sym( SoMod *m, size_t si, int *missing, char *names, size_t namesz )
{
	const Sym	*s;
	const char	*name;
	void		*p;

	if ( si == 0 ) return 0;
	if ( si >= m->nsyms ) {
		*missing = 1;
		return 0;
	}
	s = &m->symtab[si];
	name = str_at(m, s->st_name);
	if ( s->st_shndx != SHN_UNDEF ) {
		uintptr_t a = m->bias + s->st_value;
		if ( ( s->st_info & 0xf ) == STT_GNU_IFUNC ) {
			a = ((uintptr_t (*)( unsigned long, void * ))a)(hwcap_of(), NULL);
		}
		return a;
	}
	p = ts_glibc_sym(name, version_of(m, si));
	if ( p != NULL ) return (uintptr_t)p;
	if ( ( s->st_info >> 4 ) == STB_WEAK ) return 0;
	*missing = 1;
	/* each said, so that one try names every call the process lacks */
	printf("browser: so: %s: no symbol %s (%s)\n", m->name, name, version_of(m, si) ? version_of(m, si) : "-");
	if ( strlen(names) + strlen(name) + 3 < namesz ) {
		if ( names[0] != 0 ) strcat(names, ", ");
		strcat(names, name);
	}
	return 0;
}

/* ---------------------------------------------------------------- loading */

static int in_image( size_t size, uint64_t off, uint64_t len )
{
	return off <= size && len <= size - off;
}

static int known_mod( const void *h )
{
	SoMod *m;
	for ( m = so_list; m != NULL; m = m->next ) {
		if ( (const void *)m == h ) return 1;
	}
	return 0;
}

int ts_so_is_handle( const void *h )
{
	int ok;
	pthread_mutex_lock(&so_lock);
	ok = h != NULL && known_mod(h);
	pthread_mutex_unlock(&so_lock);
	return ok;
}

void *ts_so_find( const char *name )
{
	SoMod *m;

	if ( name == NULL ) return NULL;
	pthread_mutex_lock(&so_lock);
	for ( m = so_list; m != NULL; m = m->next ) {
		if ( strcmp(m->name, name) == 0 ) {
			m->refs++;
			break;
		}
	}
	pthread_mutex_unlock(&so_lock);
	return m;
}

/* Every relocation of a RELA table applied */
static int apply_rela( SoMod *m, const Rela *r, size_t n, char *err, size_t errsz )
{
	char		names[256] = "";
	int		missing = 0;
	uintptr_t	lo = (uintptr_t)m->map - m->bias;	/* the object's lowest address */
	size_t		i;

	for ( i = 0; i < n; i++ ) {
		uint64_t	*where = (uint64_t *)( m->bias + r[i].r_offset );
		uint32_t	type = (uint32_t)r[i].r_info;
		size_t		si = (size_t)( r[i].r_info >> 32 );
		uintptr_t	a;

		if ( r[i].r_offset < lo || r[i].r_offset + 16 > lo + m->mapsz ) {
			fail(err, errsz, "%s: a relocation outside the object (%ld)", m->name, (long)i);
			return -1;
		}
		switch ( type ) {
		case R_NONE:
			break;
		case R_RELATIVE:
			*where = m->bias + (uint64_t)r[i].r_addend;
			break;
		case R_ABS64:
		case R_GLOB_DAT:
		case R_JUMP_SLOT:
			a = bind_sym(m, si, &missing, names, sizeof(names));
			*where = ( a == 0 && si != 0 ) ? 0 : a + (uint64_t)r[i].r_addend;
			break;
		case R_IRELATIVE:
			*where = ((uintptr_t (*)( unsigned long, void * ))( m->bias + (uint64_t)r[i].r_addend ))(hwcap_of(), NULL);
			break;
		case R_TLSDESC:
		case R_DTPMOD64:
		case R_DTPREL64: {
			uint64_t off = (uint64_t)r[i].r_addend;
			if ( si != 0 ) {
				const Sym *s = &m->symtab[si < m->nsyms ? si : 0];
				if ( s->st_shndx == SHN_UNDEF ) {
					fail(err, errsz, "%s: a thread-local variable of another object", m->name);
					return -1;
				}
				off += s->st_value;
			}
			if ( !m->has_tls ) {
				fail(err, errsz, "%s: a TLS relocation without a TLS segment (%ld)", m->name, (long)i);
				return -1;
			}
			if ( type == R_DTPMOD64 ) {
				*where = m->tls_modid;
			} else if ( type == R_DTPREL64 ) {
				*where = off;
			} else {
				struct so_tls *t = (struct so_tls *)calloc(1, sizeof(*t));
				if ( t == NULL ) {
					fail(err, errsz, "%s: no memory", m->name);
					return -1;
				}
				t->mod = m;
				t->off = off;
				t->next = m->tls_args;
				m->tls_args = t;
				where[0] = (uint64_t)(uintptr_t)ts_so_tlsdesc;
				where[1] = (uint64_t)(uintptr_t)t;
			}
			break;
		}
		case R_TPREL64:
			/* the initial-exec model needs a place fixed in every thread's block */
			fail(err, errsz, "%s: initial-exec thread-local storage is not supported (%ld)", m->name, (long)i);
			return -1;
		default:
			fail(err, errsz, "%s: relocation type %ld not supported", m->name, (long)type);
			return -1;
		}
	}
	if ( missing ) {
		fail(err, errsz, "%s: symbols the process does not have: %s", m->name, names);
		return -1;
	}
	return 0;
}

/* The relative relocations of DT_RELR: an address, then bitmaps of the words after it */
static void apply_relr( SoMod *m, const uint64_t *r, size_t n )
{
	uint64_t	*where = NULL;
	size_t		i;

	for ( i = 0; i < n; i++ ) {
		uint64_t e = r[i];
		if ( ( e & 1 ) == 0 ) {
			where = (uint64_t *)( m->bias + e );
			*where++ += m->bias;
		} else {
			uint64_t *w = where;
			for ( e >>= 1; e != 0; e >>= 1, w++ ) {
				if ( e & 1 ) *w += m->bias;
			}
			where += 63;
		}
	}
}

/* libgcc's registry of call frame information (unwind-dw2-fde.c) */
extern void	__register_frame_info_table_bases( void *begin, void *ob, void *tbase, void *dbase );
extern void	*__deregister_frame_info( const void *begin );
#define EH_OBJECT_WORDS	16	/* room for libgcc's struct object */

/* A value of .eh_frame_hdr in the encodings its table uses; false for another */
static int eh_read( const unsigned char **pp, unsigned char enc, uintptr_t hdr, uintptr_t *v )
{
	const unsigned char	*p = *pp;
	uintptr_t		base = 0;
	int64_t			x;

	if ( enc == 0xff ) return 0;			/* DW_EH_PE_omit */
	switch ( enc & 0x70 ) {
	case 0x00:	base = 0; break;			/* absolute */
	case 0x10:	base = (uintptr_t)p; break;		/* pcrel */
	case 0x30:	base = hdr; break;			/* datarel: from the table's start */
	default:	return 0;
	}
	switch ( enc & 0x0f ) {
	case 0x03:	{ uint32_t u; memcpy(&u, p, 4); x = u; p += 4; break; }	/* udata4 */
	case 0x0b:	{ int32_t i; memcpy(&i, p, 4); x = i; p += 4; break; }	/* sdata4 */
	case 0x04:	case 0x0c:	{ memcpy(&x, p, 8); p += 8; break; }	/* udata8, sdata8 */
	default:	return 0;
	}
	*v = base + (uintptr_t)x;
	*pp = p;
	return 1;
}

/*
 * The object's FDEs registered with the process's unwinder, from its
 * .eh_frame_hdr (version 1: the encoding of .eh_frame's address, of the
 * count and of the sorted table of the initial locations and the FDEs)
 */
static void eh_register( SoMod *m, const Phdr *ph, int phnum )
{
	const unsigned char	*h = NULL, *p;
	uintptr_t		ehf, n, loc, fde, k;
	int			i;

	for ( i = 0; i < phnum; i++ ) {
		if ( ph[i].p_type == PT_GNU_EH_FRAME ) h = (const unsigned char *)( m->bias + ph[i].p_vaddr );
	}
	if ( h == NULL || h[0] != 1 ) {
		printf("browser: so: %s: no frame table (PT_GNU_EH_FRAME) for the unwinder\n", m->name);
		return;
	}
	p = h + 4;
	if ( !eh_read(&p, h[1], (uintptr_t)h, &ehf) || !eh_read(&p, h[2], (uintptr_t)h, &n) || n == 0 || n > ( 1u << 24 ) ) {
		printf("browser: so: %s: a frame table of encodings %u, %u not read\n", m->name, h[1], h[2]);
		return;
	}
	m->fdes = (const void **)calloc((size_t)n + 1, sizeof(void *));
	m->eh_object = calloc(EH_OBJECT_WORDS, sizeof(void *));
	if ( m->fdes == NULL || m->eh_object == NULL ) {
		free(m->fdes);
		free(m->eh_object);
		m->fdes = NULL;
		m->eh_object = NULL;
		return;
	}
	for ( k = 0; k < n; k++ ) {
		if ( !eh_read(&p, h[3], (uintptr_t)h, &loc) || !eh_read(&p, h[3], (uintptr_t)h, &fde)
		     || fde < (uintptr_t)m->map || fde >= (uintptr_t)m->map + m->mapsz ) {
			printf("browser: so: %s: the frame table's entry %lu not read (encoding %u)\n", m->name,
			       (unsigned long)k, h[3]);
			free(m->fdes);
			free(m->eh_object);
			m->fdes = NULL;
			m->eh_object = NULL;
			return;
		}
		m->fdes[k] = (const void *)fde;
	}
	__register_frame_info_table_bases((void *)m->fdes, m->eh_object, NULL, NULL);
	printf("browser: so: %s: %lu FDEs made known to the unwinder\n", m->name, (unsigned long)n);
}

static void unmap( SoMod *m )
{
	struct so_tls *t, *n;

	if ( m->fdes != NULL ) {
		(void)__deregister_frame_info(m->fdes);
		free(m->fdes);
		free(m->eh_object);
	}
	if ( m->map != NULL ) (void)ts_unm_mem(m->map, (long)m->mapsz);
	for ( t = m->tls_args; t != NULL; t = n ) {
		n = t->next;
		free(t);
	}
	if ( m->has_tls ) (void)pthread_key_delete(m->tls_key);
	free(m);
}

void *ts_so_load( const char *name, const void *image, size_t size, char *err, size_t errsz )
{
	const unsigned char	*img = (const unsigned char *)image;
	const Ehdr		*eh = (const Ehdr *)image;
	const Phdr		*ph;
	uintptr_t		lo = UINTPTR_MAX, hi = 0;
	SoMod			*m;
	void			*p = NULL;
	int			i;

	if ( errsz > 0 ) err[0] = 0;
	if ( (m = (SoMod *)ts_so_find(name)) != NULL ) return m;

	if ( size < sizeof(Ehdr) || memcmp(eh->e_ident, "\177ELF", 4) != 0 || eh->e_ident[4] != 2
	     || eh->e_ident[5] != 1 ) {
		fail(err, errsz, "%s: not a 64-bit little-endian ELF file", name);
		return NULL;
	}
	if ( eh->e_type != ET_DYN || eh->e_machine != EM_THIS || eh->e_phentsize != sizeof(Phdr)
	     || !in_image(size, eh->e_phoff, (uint64_t)eh->e_phnum * sizeof(Phdr)) ) {
		fail(err, errsz, "%s: not an " MACHINE " shared object (type %ld, machine %ld)", name, (long)eh->e_type, (long)eh->e_machine);
		return NULL;
	}
	ph = (const Phdr *)( img + eh->e_phoff );
	for ( i = 0; i < eh->e_phnum; i++ ) {
		if ( ph[i].p_type != PT_LOAD ) continue;
		if ( !in_image(size, ph[i].p_offset, ph[i].p_filesz) || ph[i].p_filesz > ph[i].p_memsz
		     || ( ph[i].p_offset % PAGE ) != ( ph[i].p_vaddr % PAGE ) ) {
			fail(err, errsz, "%s: a segment that does not fit its file (%ld)", name, (long)i);
			return NULL;
		}
		if ( PG_DOWN(ph[i].p_vaddr) < lo ) lo = PG_DOWN(ph[i].p_vaddr);
		if ( PG_UP(ph[i].p_vaddr + ph[i].p_memsz) > hi ) hi = PG_UP(ph[i].p_vaddr + ph[i].p_memsz);
	}
	if ( hi <= lo || hi - lo > ( 1UL << 30 ) ) {
		fail(err, errsz, "%s: no loadable segments, or too large (%ld)", name, (long)( hi - lo ));
		return NULL;
	}

	m = (SoMod *)calloc(1, sizeof(*m));
	if ( m == NULL ) {
		fail(err, errsz, "%s: no memory", name);
		return NULL;
	}
	snprintf(m->name, sizeof(m->name), "%s", name);
	m->refs = 1;
	m->mapsz = hi - lo;
	if ( ts_map_mem(&p, (long)m->mapsz, 0x10000, TS_MEM_NONE) < 0
	     || ts_ctl_mem(p, (long)m->mapsz, TS_MEM_RW) < 0 ) {
		if ( p != NULL ) (void)ts_unm_mem(p, (long)m->mapsz);
		free(m);
		fail(err, errsz, "%s: no room for %ld bytes", name, (long)( hi - lo ));
		return NULL;
	}
	m->map = (unsigned char *)p;
	m->bias = (uintptr_t)p - lo;

	/* the segments in their places; the pages came zero filled */
	for ( i = 0; i < eh->e_phnum; i++ ) {
		if ( ph[i].p_type == PT_LOAD ) {
			memcpy((void *)( m->bias + ph[i].p_vaddr ), img + ph[i].p_offset, ph[i].p_filesz);
		}
	}
	/* the program headers as loaded, when they are in a segment; else a copy */
	m->phnum = eh->e_phnum;
	for ( i = 0; i < eh->e_phnum; i++ ) {
		if ( ph[i].p_type == PT_LOAD && eh->e_phoff >= ph[i].p_offset
		     && eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Phdr) <= ph[i].p_offset + ph[i].p_filesz ) {
			m->phdr = (const Phdr *)( m->bias + ph[i].p_vaddr + ( eh->e_phoff - ph[i].p_offset ) );
		}
	}
	if ( m->phdr == NULL ) {
		Phdr *c = (Phdr *)malloc((size_t)eh->e_phnum * sizeof(Phdr));
		if ( c == NULL ) {
			unmap(m);
			fail(err, errsz, "%s: no memory", name);
			return NULL;
		}
		memcpy(c, ph, (size_t)eh->e_phnum * sizeof(Phdr));
		m->phdr = c;	/* kept for the life of the process */
	}

	/* the dynamic section */
	{
		const uint32_t	*hash = NULL, *gnu = NULL;
		const Rela	*rela = NULL, *jmprel = NULL;
		const uint64_t	*relr = NULL;
		size_t		relasz = 0, jmprelsz = 0, relrsz = 0, init_n = 0;
		void		(*init)( void ) = NULL;
		void		(**init_array)( int, char **, char ** ) = NULL;
		uintptr_t	vn = 0;
		size_t		vnnum = 0;
		const Dyn	*d;

		for ( i = 0; i < eh->e_phnum; i++ ) {
			if ( ph[i].p_type == PT_DYNAMIC ) m->dyn = (const Dyn *)( m->bias + ph[i].p_vaddr );
			if ( ph[i].p_type == PT_TLS ) {
				m->has_tls = 1;
				m->tls_image = (const unsigned char *)( m->bias + ph[i].p_vaddr );
				m->tls_filesz = ph[i].p_filesz;
				m->tls_memsz = ph[i].p_memsz;
				m->tls_align = ph[i].p_align ? ph[i].p_align : 1;
			}
		}
		if ( m->dyn == NULL ) {
			unmap(m);
			fail(err, errsz, "%s: no dynamic section", name);
			return NULL;
		}
		for ( d = m->dyn; d->d_tag != DT_NULL; d++ ) {
			uintptr_t a = m->bias + d->d_val;
			switch ( d->d_tag ) {
			case DT_HASH:		hash = (const uint32_t *)a; break;
			case DT_GNU_HASH:	gnu = (const uint32_t *)a; break;
			case DT_STRTAB:		m->strtab = (const char *)a; break;
			case DT_STRSZ:		m->strsz = d->d_val; break;
			case DT_SYMTAB:		m->symtab = (const Sym *)a; break;
			case DT_RELA:		rela = (const Rela *)a; break;
			case DT_RELASZ:		relasz = d->d_val; break;
			case DT_JMPREL:		jmprel = (const Rela *)a; break;
			case DT_PLTRELSZ:	jmprelsz = d->d_val; break;
			case DT_RELR:		relr = (const uint64_t *)a; break;
			case DT_RELRSZ:		relrsz = d->d_val; break;
			case DT_INIT:		init = (void (*)( void ))a; break;
			case DT_FINI:		m->fini = (void (*)( void ))a; break;
			case DT_INIT_ARRAY:	init_array = (void (**)( int, char **, char ** ))a; break;
			case DT_INIT_ARRAYSZ:	init_n = d->d_val / sizeof(void *); break;
			case DT_FINI_ARRAY:	m->fini_array = (void (**)( void ))a; break;
			case DT_FINI_ARRAYSZ:	m->fini_n = d->d_val / sizeof(void *); break;
			case DT_VERSYM:		m->versym = (const uint16_t *)a; break;
			case DT_VERNEED:	vn = a; break;
			case DT_VERNEEDNUM:	vnnum = d->d_val; break;
			case DT_REL:
			case DT_TEXTREL:
				unmap(m);
				fail(err, errsz, "%s: relocations of a kind not made for " MACHINE " (tag %ld)", name, (long)d->d_tag);
				return NULL;
			case DT_PLTREL:
				if ( d->d_val != DT_RELA ) {
					unmap(m);
					fail(err, errsz, "%s: PLT relocations not RELA", name);
					return NULL;
				}
				break;
			default:
				break;
			}
		}
		if ( m->strtab == NULL || m->symtab == NULL ) {
			unmap(m);
			fail(err, errsz, "%s: no symbol table", name);
			return NULL;
		}
		m->nsyms = count_syms(m, hash, gnu);

		/* the libraries it needs, and the versions of their symbols */
		int unwinder = 0;
		for ( d = m->dyn; d->d_tag != DT_NULL; d++ ) {
			if ( d->d_tag == DT_NEEDED && strcmp(str_at(m, d->d_val), "libgcc_s.so.1") == 0 ) unwinder = 1;
			if ( d->d_tag == DT_NEEDED && !ts_glibc_library_ok(str_at(m, d->d_val)) ) {
				fail(err, errsz, "%s: needs %s, which the process does not have", name, str_at(m, d->d_val));
				unmap(m);
				return NULL;
			}
		}
		while ( vn != 0 && vnnum-- > 0 ) {
			const Verneed *v = (const Verneed *)vn;
			uintptr_t a = vn + v->vn_aux;
			for ( uint16_t k = 0; k < v->vn_cnt; k++ ) {
				const Vernaux *x = (const Vernaux *)a;
				if ( ( x->vna_flags & VER_FLG_WEAK ) && !ts_glibc_version_ok(str_at(m, x->vna_name)) ) {
					/* a weak need: what it names may be missing, as the dynamic linker allows */
					printf("browser: so: %s: the weak need of '%s' of %s is not met; it is let be\n",
					       name, str_at(m, x->vna_name), str_at(m, v->vn_file));
				} else if ( !ts_glibc_version_ok(str_at(m, x->vna_name)) ) {
					snprintf(err, errsz, "%s: asks for '%s' of %s (flags %u), which the process does not have",
						 name, str_at(m, x->vna_name), str_at(m, v->vn_file), (unsigned)x->vna_flags);
					snprintf(so_err, sizeof(so_err), "%s", err);
					so_err_set = 1;
					printf("browser: so: %s\n", err);
					unmap(m);
					return NULL;
				}
				if ( x->vna_next == 0 ) break;
				a += x->vna_next;
			}
			if ( v->vn_next == 0 ) break;
			vn += v->vn_next;
		}

		if ( m->has_tls ) {
			if ( pthread_key_create(&m->tls_key, tls_free) != 0 ) {
				unmap(m);
				fail(err, errsz, "%s: no thread key for its TLS", name);
				return NULL;
			}
			pthread_mutex_lock(&so_lock);
			m->tls_modid = so_next_modid++;
			pthread_mutex_unlock(&so_lock);
		}

		if ( relr != NULL ) apply_relr(m, relr, relrsz / sizeof(uint64_t));
		if ( ( rela != NULL && apply_rela(m, rela, relasz / sizeof(Rela), err, errsz) < 0 )
		     || ( jmprel != NULL && apply_rela(m, jmprel, jmprelsz / sizeof(Rela), err, errsz) < 0 ) ) {
			unmap(m);
			return NULL;
		}

		/* the protections: code run and not written, what relocation made read-only read-only */
		for ( i = 0; i < eh->e_phnum; i++ ) {
			uintptr_t s = PG_DOWN(m->bias + ph[i].p_vaddr), e = PG_UP(m->bias + ph[i].p_vaddr + ph[i].p_memsz);
			unsigned prot;
			if ( ph[i].p_type != PT_LOAD || ( ph[i].p_flags & PF_W ) ) continue;
			prot = ( ph[i].p_flags & PF_X ) ? ( TS_MEM_READ | TS_MEM_EXEC ) : TS_MEM_READ;
			if ( ts_ctl_mem((void *)s, (long)( e - s ), prot) < 0 ) {
				unmap(m);
				fail(err, errsz, "%s: its code cannot be made executable (%ld)", name, (long)i);
				return NULL;
			}
		}
		for ( i = 0; i < eh->e_phnum; i++ ) {
			uintptr_t s, e;
			if ( ph[i].p_type != PT_GNU_RELRO ) continue;
			s = PG_DOWN(m->bias + ph[i].p_vaddr);
			e = PG_DOWN(m->bias + ph[i].p_vaddr + ph[i].p_memsz);
			if ( e > s ) (void)ts_ctl_mem((void *)s, (long)( e - s ), TS_MEM_READ);
		}

		pthread_mutex_lock(&so_lock);
		m->next = so_list;
		so_list = m;
		so_adds++;
		pthread_mutex_unlock(&so_lock);

		printf("browser: so: %s loaded at %p, %lu KB, %lu symbols%s\n", name, (void *)m->map,
		       (unsigned long)( m->mapsz >> 10 ), (unsigned long)m->nsyms, m->has_tls ? ", TLS" : "");

		/* its frames known to the unwinder, before any of its code runs */
		if ( unwinder ) eh_register(m, ph, eh->e_phnum);

		/* its initialising functions, as the dynamic linker runs them */
		{
			static char	*argv[] = { (char *)"browser", NULL };
			static char	*envp[] = { NULL };
			size_t		k;
			if ( init != NULL ) init();
			for ( k = 0; k < init_n; k++ ) {
				if ( init_array[k] != NULL && (uintptr_t)init_array[k] != (uintptr_t)-1 ) {
					init_array[k](1, argv, envp);
				}
			}
		}
	}
	return m;
}

void *ts_so_sym( void *h, const char *sym )
{
	void *p;

	if ( !ts_so_is_handle(h) ) return NULL;
	p = own_sym((SoMod *)h, sym);
	if ( p == NULL ) {
		snprintf(so_err, sizeof(so_err), "%s: no symbol %s", ((SoMod *)h)->name, sym);
		so_err_set = 1;
	}
	return p;
}

int ts_so_close( void *h )
{
	SoMod	*m = (SoMod *)h, **pp;
	size_t	k;

	pthread_mutex_lock(&so_lock);
	if ( !known_mod(h) ) {
		pthread_mutex_unlock(&so_lock);
		return -1;
	}
	if ( --m->refs > 0 ) {
		pthread_mutex_unlock(&so_lock);
		return 0;
	}
	pthread_mutex_unlock(&so_lock);

	/* its ending functions: what __cxa_atexit registered for it, then its own */
	ts_glibc_finalize(m->map);
	for ( k = m->fini_n; k > 0; k-- ) {
		if ( m->fini_array[k - 1] != NULL && (uintptr_t)m->fini_array[k - 1] != (uintptr_t)-1 ) {
			m->fini_array[k - 1]();
		}
	}
	if ( m->fini != NULL ) m->fini();

	pthread_mutex_lock(&so_lock);
	for ( pp = &so_list; *pp != NULL; pp = &( *pp )->next ) {
		if ( *pp == m ) {
			*pp = m->next;
			break;
		}
	}
	so_subs++;
	pthread_mutex_unlock(&so_lock);
	printf("browser: so: %s let go\n", m->name);
	unmap(m);
	return 0;
}

int ts_so_addr( const void *addr, const char **name, void **base )
{
	SoMod *m;
	int found = 0;

	pthread_mutex_lock(&so_lock);
	for ( m = so_list; m != NULL; m = m->next ) {
		if ( (const unsigned char *)addr >= m->map && (const unsigned char *)addr < m->map + m->mapsz ) {
			*name = m->name;
			*base = (void *)m->bias;
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&so_lock);
	return found;
}

/* The GNU C library's struct dl_phdr_info (the same on aarch64 and x86-64) */
struct glibc_phdr_info {
	uintptr_t		dlpi_addr;
	const char		*dlpi_name;
	const Phdr		*dlpi_phdr;
	uint16_t		dlpi_phnum;
	unsigned long long	dlpi_adds;
	unsigned long long	dlpi_subs;
	size_t			dlpi_tls_modid;
	void			*dlpi_tls_data;
};

int ts_so_iterate( int (*cb)( void *info, size_t size, void *data ), void *data )
{
	SoMod	*m;
	int	r = 0;

	/* the list is walked unlocked: an object is let go only when nothing of it runs */
	for ( m = so_list; m != NULL && r == 0; m = m->next ) {
		struct glibc_phdr_info i;
		i.dlpi_addr = m->bias;
		i.dlpi_name = m->name;
		i.dlpi_phdr = m->phdr;
		i.dlpi_phnum = (uint16_t)m->phnum;
		i.dlpi_adds = so_adds;
		i.dlpi_subs = so_subs;
		i.dlpi_tls_modid = m->has_tls ? m->tls_modid : 0;
		i.dlpi_tls_data = m->has_tls ? pthread_getspecific(m->tls_key) : NULL;
		r = cb(&i, sizeof(i), data);
	}
	return r;
}

#else

/*
 * Another processor: no GNU C library's objects are made for it here;
 * nothing is loaded, and the CDM is said not to be there.
 */
static const char	so_none[] = "no shared objects on this processor";

void *ts_so_load( const char *name, const void *image, size_t size, char *err, size_t errsz )
{
	(void)name; (void)image; (void)size;
	if ( err != NULL && errsz > 0 ) {
		strncpy(err, so_none, errsz - 1);
		err[errsz - 1] = '\0';
	}
	return NULL;
}

void *ts_so_find( const char *name )
{
	(void)name;
	return NULL;
}

int ts_so_is_handle( const void *h )
{
	(void)h;
	return 0;
}

void *ts_so_sym( void *h, const char *sym )
{
	(void)h; (void)sym;
	return NULL;
}

int ts_so_close( void *h )
{
	(void)h;
	return -1;
}

const char *ts_so_error( void )
{
	return NULL;
}

int ts_so_addr( const void *addr, const char **name, void **base )
{
	(void)addr; (void)name; (void)base;
	return 0;
}

int ts_so_iterate( int (*cb)( void *info, size_t size, void *data ), void *data )
{
	(void)cb; (void)data;
	return 0;
}

#endif /* __aarch64__ || __x86_64__ */

/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	link.h
 *	The program headers of what is loaded (dl_iterate_phdr): a TessronOS
 *	program is one static image, reported as such.
 */

#ifndef __TS_LINK_H__
#define __TS_LINK_H__

#include <stddef.h>
#include <stdint.h>
#include <elf.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ElfW
#define ElfW(type)	Elf64_##type
#endif

struct dl_phdr_info {
	ElfW(Addr)		dlpi_addr;
	const char		*dlpi_name;
	const ElfW(Phdr)	*dlpi_phdr;
	ElfW(Half)		dlpi_phnum;
	unsigned long long	dlpi_adds;
	unsigned long long	dlpi_subs;
	size_t			dlpi_tls_modid;
	void			*dlpi_tls_data;
};

int	dl_iterate_phdr( int (*cb)( struct dl_phdr_info *info, size_t size, void *data ),
			 void *data );

#ifdef __cplusplus
}
#endif

#endif /* __TS_LINK_H__ */

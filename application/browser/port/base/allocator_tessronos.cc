/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	allocator_tessronos.cc
 *	The allocator's unchecked calls (base::UncheckedMalloc and the like)
 *	on the C library's malloc (design 17.19). The allocator shim that
 *	would route malloc through PartitionAlloc is not built: malloc stays
 *	newlib's over lib/libcxxrt's _sbrk, and these answer null where
 *	there is no memory rather than stopping the program.
 */

#include <stdlib.h>

#include "partition_alloc/shim/allocator_shim.h"

namespace allocator_shim {

void *UncheckedAlloc( size_t size )
{
	return malloc(size);
}

void *UncheckedCalloc( size_t n, size_t size )
{
	return calloc(n, size);
}

void *UncheckedRealloc( void *ptr, size_t size )
{
	return realloc(ptr, size);
}

void UncheckedFree( void *ptr )
{
	free(ptr);
}

void *UncheckedAlignedAlloc( size_t size, size_t align )
{
	void	*p = nullptr;

	return ( posix_memalign(&p, align, size) == 0 ) ? p : nullptr;
}

void UncheckedAlignedFree( void *ptr )
{
	free(ptr);
}

}  // namespace allocator_shim

/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	base_tessronos.cc
 *	What //base asks of the platform and TessronOS answers itself (design
 *	17.19): whether this is a test (never), and the text of a stack
 *	trace (a process has no unwinder that walks its own stack from a
 *	signal, so a trace is not printed).
 */

#include <ostream>

#include "base/check_is_test.h"
#include "base/debug/stack_trace.h"

namespace base {

namespace internal {

bool get_is_test_impl()
{
	return false;
}

}  // namespace internal

namespace debug {

void StackTrace::OutputToStreamWithPrefixImpl( std::ostream *os,
					       cstring_view prefix_string ) const
{
	*os << prefix_string << "(no stack trace on this system)\n";
}

}  // namespace debug

}  // namespace base

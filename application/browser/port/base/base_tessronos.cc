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
 *	17.19): whether this is a test (never), the text of a stack trace (a
 *	process has no unwinder that walks its own stack from a signal, so a
 *	trace is not printed), and how a thread's type is set.
 */

#include <sched.h>

#include <ostream>

#include "base/check_is_test.h"
#include "base/debug/stack_trace.h"
#include "base/threading/platform_thread.h"
#include "base/threading/platform_thread_internal_posix.h"
#include "base/threading/thread_type_delegate.h"

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

namespace {

/*
 * A thread's type set as on Linux, but for the control groups: TessronOS
 * has none, and //base would look for their directories under
 * /sys/fs/cgroup each time a thread's type is set, a path a renderer under
 * its restrictions may not use (design 14.13.9). The real-time audio thread
 * asks for SCHED_RR first; every type has its nice value.
 */
class ThreadTypes final : public base::ThreadTypeDelegate {
public:
	bool HandleThreadTypeChange( base::PlatformThreadId thread_id, base::ThreadType thread_type ) override
	{
		if ( thread_type == base::ThreadType::kRealtimeAudio
		     && sched_setscheduler(0, SCHED_RR, &base::PlatformThreadLinux::kRealTimeAudioPrio) == 0 ) {
			return true;
		}
		(void)base::internal::SetThreadNiceFromType(thread_id, thread_type);
		return true;
	}
};

}  // namespace

/* Once, before the process's threads are started (br_rproc.cc) */
void br_thread_types_own()
{
	static ThreadTypes types;
	base::PlatformThreadLinux::SetThreadTypeDelegate(&types);
}

/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_cpu.cc
 *	ブラウザ: how busy the machine's processors are (Compute Pressure,
 *	devices_tessronos.cc), from the time the kernel says each has spent
 *	running tasks (ts_ref_cpu, design 4.3). The kernel is told the size
 *	of what it writes into, so a machine of any number of processors is
 *	read whole: the number first, then a buffer of TS_TSCPU_SIZE of it.
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/time.h>

#include <mutex>
#include <vector>

namespace {

std::mutex			g_lock;
std::vector<unsigned char>	g_last;		/* the last answer, a T_TSCPU and its busy[] */

/* The processors and their time now, into buf: how many processors' times it holds; 0 when not said */
int read_cpu( std::vector<unsigned char> &buf )
{
	T_TSCPU	head;
	int	n = ts_ref_cpu(&head, (SZ)sizeof(head));

	if ( n <= 0 ) return 0;
	buf.assign((size_t)TS_TSCPU_SIZE(n), 0);
	T_TSCPU	*now = reinterpret_cast<T_TSCPU *>(buf.data());
	int	m = ts_ref_cpu(now, (SZ)buf.size());

	if ( m <= 0 || now->ncpu <= 0 ) return 0;
	return ( now->ncpu < n ) ? now->ncpu : n;	/* one gone offline since: what was written */
}

}  // namespace

/*
 * The share of the processors' time spent running tasks since the last
 * call (0 to 1), and the processors online; false the first time, or
 * when the kernel does not say
 */
extern "C" bool br_cpu_load( double *load, int *ncpu )
{
	std::vector<unsigned char> buf;
	int	m = read_cpu(buf);

	if ( m <= 0 ) return false;
	const T_TSCPU *now = reinterpret_cast<const T_TSCPU *>(buf.data());
	std::lock_guard<std::mutex> hold(g_lock);
	const T_TSCPU *last = g_last.empty() ? nullptr : reinterpret_cast<const T_TSCPU *>(g_last.data());
	bool ok = last != nullptr && now->now > last->now && last->ncpu == now->ncpu;
	if ( ok ) {
		UD busy = 0;
		for ( int i = 0; i < m; i++ ) busy += now->busy[i] - last->busy[i];
		double l = (double)busy / ( (double)( now->now - last->now ) * m );
		*load = l < 0 ? 0 : l > 1 ? 1 : l;
	}
	*ncpu = now->ncpu;
	g_last.swap(buf);
	return ok;
}

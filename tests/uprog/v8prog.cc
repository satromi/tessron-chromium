/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	v8prog.cc
 *	V8 as a TessronOS process (design 17.19): the engine is started without
 *	a compiler to machine code (--jitless) and runs a script that leans
 *	on the parts a page leans on -- closures and arrows, arrays and their
 *	sort, JSON, regular expressions, BigInt, classes and generators,
 *	Date, the collation and number formats of Intl, a loop long enough to
 *	be measured, and the garbage collector. The script's answer is
 *	printed on the console and compared with what it must be; the exit
 *	code is 0 when it matches. Built by tools/browser/build.sh
 *	(make BROWSER=1), run by tests/ktest/ktest_v8.c.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <memory>
#include <string>

#include "include/libplatform/libplatform.h"
#include "include/v8-context.h"
#include "include/v8-initialization.h"
#include "include/v8-isolate.h"
#include "include/v8-local-handle.h"
#include "include/v8-primitive.h"
#include "include/v8-script.h"

static const char kScript[] =
    "function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }\n"
    "class P { constructor(x) { this.x = x; } get twice() { return this.x * 2; } }\n"
    "function* gen() { yield 1; yield 2; yield 3; }\n"
    "const parts = [];\n"
    "parts.push([5, 3, 8, 1, 9, 2].sort((a, b) => a - b).join(','));\n"
    "parts.push([1, 2, 3, 4].map(x => x * x).reduce((s, x) => s + x, 0));\n"
    "parts.push(JSON.stringify({ a: 1, b: [true, null, 'x'] }));\n"
    "parts.push('TessronOS browser'.replace(/(\\w+)$/, m => m.toUpperCase()));\n"
    "parts.push((2n ** 100n).toString().length);\n"
    "parts.push(new P(21).twice);\n"
    "parts.push([...gen()].join(''));\n"
    "parts.push(fib(22));\n"
    "parts.push(new Date(Date.UTC(2026, 8, 28)).toISOString());\n"
    "parts.push(new Intl.NumberFormat('en-US').format(1234567.891));\n"
    "parts.push(['b', 'a', 'c'].sort(new Intl.Collator('en').compare).join(''));\n"
    "let junk = []; for (let i = 0; i < 20000; i++) junk.push({ i, s: 'x' + i });\n"
    "parts.push(junk.length);\n"
    "parts.join(' | ');\n";

static const char kExpect[] =
    "1,2,3,5,8,9 | 30 | {\"a\":1,\"b\":[true,null,\"x\"]} | TessronOS BROWSER | 31 | 42 | 123 | "
    "17711 | 2026-09-28T00:00:00.000Z | 1,234,567.891 | abc | 20000";

static long ms_since( const struct timespec *t0 )
{
	struct timespec t1;

	clock_gettime(CLOCK_MONOTONIC, &t1);
	return ( t1.tv_sec - t0->tv_sec ) * 1000 + ( t1.tv_nsec - t0->tv_nsec ) / 1000000;
}

int main()
{
	struct timespec	t0;
	int		rc = 1;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	const char flags[] = "--jitless --single-threaded --stack-size=900";
	v8::V8::SetFlagsFromString(flags, sizeof(flags) - 1);
	v8::V8::InitializeICU();
	std::unique_ptr<v8::Platform> platform = v8::platform::NewSingleThreadedDefaultPlatform();
	v8::V8::InitializePlatform(platform.get());
	v8::V8::Initialize();
	printf("v8prog: V8 %s up in %ld ms\n", v8::V8::GetVersion(), ms_since(&t0));

	v8::Isolate::CreateParams params;
	params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
	v8::Isolate *isolate = v8::Isolate::New(params);
	{
		v8::Isolate::Scope		iscope(isolate);
		v8::HandleScope			hscope(isolate);
		v8::Local<v8::Context>		context = v8::Context::New(isolate);
		v8::Context::Scope		cscope(context);
		struct timespec			t1;

		printf("v8prog: context in %ld ms\n", ms_since(&t0));
		clock_gettime(CLOCK_MONOTONIC, &t1);
		v8::Local<v8::String> src =
		    v8::String::NewFromUtf8(isolate, kScript).ToLocalChecked();
		v8::Local<v8::Script> script;
		v8::Local<v8::Value> result;
		if ( v8::Script::Compile(context, src).ToLocal(&script)
		  && script->Run(context).ToLocal(&result) ) {
			v8::String::Utf8Value utf8(isolate, result);
			printf("v8prog: result %s\n", *utf8);
			printf("v8prog: script ran in %ld ms\n", ms_since(&t1));
			rc = ( strcmp(*utf8, kExpect) == 0 ) ? 0 : 2;
		} else {
			printf("v8prog: the script failed\n");
		}
		isolate->LowMemoryNotification();
	}
	isolate->Dispose();
	delete params.array_buffer_allocator;
	v8::V8::Dispose();
	v8::V8::DisposePlatform();
	printf("v8prog: done %d\n", rc);
	return rc;
}

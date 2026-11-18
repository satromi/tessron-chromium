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
 *	V8 as a TessronOS process (design 17.19): the engine, with its
 *	compilers to machine code, runs a script that leans on the parts a
 *	page leans on -- closures and arrows, arrays and their sort, JSON,
 *	regular expressions, BigInt, classes and generators, Date, the
 *	collation and number formats of Intl, a loop long enough to be
 *	measured, the garbage collector, and a WebAssembly module. The
 *	script's answer is printed on the console and compared with what it
 *	must be; the exit code is 0 when it matches.
 *
 *	Started with the argument "bench", it runs loops long enough for V8
 *	to compile them (recursion, a sieve over a typed array, objects,
 *	strings, and a WebAssembly loop), says how long they took and which
 *	tier the functions reached, and answers 0 when the values are right
 *	and the functions were compiled to machine code; "bench --jitless"
 *	does the same without machine code (no WebAssembly then), and
 *	answers 0 when they were interpreted. Built by tools/browser/build.sh
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
#include "include/v8-exception.h"
#include "include/v8-message.h"
#include "include/v8-script.h"

/*
 * A WebAssembly module: add(a, b); call(i), which calls entry i of a
 * table holding two functions (42 and 7); mem(x), which stores x at 8 in
 * the memory and gives back what is there plus 1; sum(n), a loop adding
 * n numbers (the i32 wraps). An entry outside the table traps.
 */
#define WASM_MODULE \
    "const wb = new Uint8Array([0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,\n" \
    " 0x01,0x10,0x03,0x60,0x02,0x7f,0x7f,0x01,0x7f,0x60,0x00,0x01,0x7f,0x60,0x01,0x7f,0x01,0x7f,\n" \
    " 0x03,0x07,0x06,0x00,0x01,0x01,0x02,0x02,0x02,\n" \
    " 0x04,0x04,0x01,0x70,0x00,0x02,\n" \
    " 0x05,0x03,0x01,0x00,0x01,\n" \
    " 0x07,0x23,0x05,0x03,0x61,0x64,0x64,0x00,0x00,0x04,0x63,0x61,0x6c,0x6c,0x00,0x03,\n" \
    " 0x03,0x6d,0x65,0x6d,0x00,0x04,0x06,0x6d,0x65,0x6d,0x6f,0x72,0x79,0x02,0x00,\n" \
    " 0x03,0x73,0x75,0x6d,0x00,0x05,\n" \
    " 0x09,0x08,0x01,0x00,0x41,0x00,0x0b,0x02,0x01,0x02,\n" \
    " 0x0a,0x4f,0x06,\n" \
    " 0x07,0x00,0x20,0x00,0x20,0x01,0x6a,0x0b,\n" \
    " 0x04,0x00,0x41,0x2a,0x0b,\n" \
    " 0x04,0x00,0x41,0x07,0x0b,\n" \
    " 0x07,0x00,0x20,0x00,0x11,0x01,0x00,0x0b,\n" \
    " 0x11,0x00,0x41,0x08,0x20,0x00,0x36,0x02,0x00,0x41,0x08,0x28,0x02,0x00,0x41,0x01,0x6a,0x0b,\n" \
    " 0x21,0x01,0x01,0x7f,0x02,0x40,0x03,0x40,0x20,0x00,0x45,0x0d,0x01,0x20,0x01,0x20,0x00,\n" \
    " 0x6a,0x21,0x01,0x20,0x00,0x41,0x01,0x6b,0x21,0x00,0x0c,0x00,0x0b,0x0b,0x20,0x01,0x0b]);\n"

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
    WASM_MODULE
    "const we = new WebAssembly.Instance(new WebAssembly.Module(wb), {}).exports;\n"
    "let trap = 'none'; try { we.call(5); } catch (e) {\n"
    " trap = e instanceof WebAssembly.RuntimeError ? 'trap' : String(e); }\n"
    "parts.push([we.add(20, 22), we.call(0), we.call(1), we.mem(41),\n"
    " new Uint32Array(we.memory.buffer)[2], we.sum(100000), trap].join(','));\n"
    "parts.join(' | ');\n";

static const char kExpect[] =
    "1,2,3,5,8,9 | 30 | {\"a\":1,\"b\":[true,null,\"x\"]} | TessronOS BROWSER | 31 | 42 | 123 | "
    "17711 | 2026-09-28T00:00:00.000Z | 1,234,567.891 | abc | 20000 | 42,42,7,42,41,705082704,trap";

/*
 * The loops of "bench", after "const JITLESS = true|false;". The answer:
 * whether the values were right, the milliseconds of the JavaScript and
 * of the WebAssembly loop (-1 without it), and the tier each function
 * reached (%GetOptimizationStatus): opt (Maglev or Turbofan), baseline
 * (Sparkplug), interp (Ignition).
 */
static const char kBench[] =
    "function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }\n"
    "function sieve(n) { const a = new Uint8Array(n + 1); let c = 0;\n"
    " for (let i = 2; i <= n; i++) { if (!a[i]) { c++; for (let j = i * i; j <= n; j += i) a[j] = 1; } }\n"
    " return c; }\n"
    "function objs(n) { let s = 0; for (let i = 0; i < n; i++) { const p = { x: i, y: i * 2 }; s += p.x + p.y; }\n"
    " return s; }\n"
    "function strs(n) { const b = []; for (let i = 0; i < n; i++) b.push(String.fromCharCode(97 + i % 26));\n"
    " const s = b.join(''); let h = 0; for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0;\n"
    " return h; }\n"
    "const t0 = Date.now();\n"
    "let ok = fib(27) === 196418;\n"
    "for (let k = 0; k < 10; k++) ok = ok && sieve(100000) === 9592;\n"
    "ok = ok && objs(500000) === 374999250000;\n"
    "ok = ok && strs(100000) === strs(100000);\n"
    "const js = Date.now() - t0;\n"
    "let wasm = -1;\n"
    "if (!JITLESS) {\n"
    WASM_MODULE
    " const we = new WebAssembly.Instance(new WebAssembly.Module(wb), {}).exports;\n"
    " const t1 = Date.now();\n"
    " for (let k = 0; k < 50; k++) ok = ok && we.sum(100000) === 705082704;\n"
    " wasm = Date.now() - t1;\n"
    "}\n"
    "const tier = f => { const s = %GetOptimizationStatus(f);\n"
    " return (s & (8 | 16 | 32)) ? 'opt' : (s & 16384) ? 'baseline' : (s & 64) ? 'interp' : 'lazy'; };\n"
    "[ok ? 'ok' : 'wrong', js, wasm, tier(fib), tier(sieve), tier(objs), tier(strs)].join(' ');\n";

static long ms_since( const struct timespec *t0 )
{
	struct timespec t1;

	clock_gettime(CLOCK_MONOTONIC, &t1);
	return ( t1.tv_sec - t0->tv_sec ) * 1000 + ( t1.tv_nsec - t0->tv_nsec ) / 1000000;
}

extern "C" long ts_get_arg( void *buf, long size );

int main()
{
	struct timespec	t0;
	int		rc = 1;
	char		arg[128] = "";
	long		n = ts_get_arg(arg, sizeof(arg) - 1);
	bool		bench = false, jitless = false;
	std::string	flags = "--single-threaded --stack-size=900";
	std::string	source = kScript;

	if ( n > 0 && n < (long)sizeof(arg) ) {
		arg[n] = '\0';
		bench = ( strncmp(arg, "bench", 5) == 0 );
		jitless = ( strstr(arg, "--jitless") != nullptr );
		if ( bench ) {
			flags += " --allow-natives-syntax ";
			flags += arg + 5;
			source = std::string("const JITLESS = ") + ( jitless ? "true" : "false" ) + ";\n" + kBench;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	printf("v8prog: flags %s\n", flags.c_str());
	v8::V8::SetFlagsFromString(flags.c_str(), flags.size());
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
		    v8::String::NewFromUtf8(isolate, source.c_str()).ToLocalChecked();
		v8::Local<v8::Script> script;
		v8::Local<v8::Value> result;
		v8::TryCatch trycatch(isolate);
		if ( v8::Script::Compile(context, src).ToLocal(&script)
		  && script->Run(context).ToLocal(&result) ) {
			v8::String::Utf8Value utf8(isolate, result);
			printf("v8prog: result %s\n", *utf8);
			printf("v8prog: script ran in %ld ms\n", ms_since(&t1));
			if ( !bench ) {
				rc = ( strcmp(*utf8, kExpect) == 0 ) ? 0 : 2;
			} else if ( strncmp(*utf8, "ok ", 3) != 0 ) {
				rc = 2;
			} else {
				/* fib and sieve: compiled to machine code, or interpreted without it */
				char	t_fib[16] = "", t_sieve[16] = "";
				long	js = 0, wasm = 0;

				sscanf(*utf8 + 3, "%ld %ld %15s %15s", &js, &wasm, t_fib, t_sieve);
				rc = jitless ? ( strcmp(t_fib, "interp") == 0 && strcmp(t_sieve, "interp") == 0 )
					     : ( strcmp(t_fib, "opt") == 0 && strcmp(t_sieve, "opt") == 0 );
				rc = rc ? 0 : 3;
			}
		} else {
			v8::String::Utf8Value why(isolate, trycatch.Exception());
			printf("v8prog: the script failed: %s (line %d)\n", *why ? *why : "?",
			       trycatch.Message().IsEmpty() ? 0
				: trycatch.Message()->GetLineNumber(context).FromMaybe(0));
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

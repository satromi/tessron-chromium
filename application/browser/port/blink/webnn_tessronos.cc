/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	webnn_tessronos.cc
 *	ブラウザ: WebNN (navigator.ml) on the processor (design 14.4.16).
 *
 *	The pages' graphs are checked and their constants laid out by
 *	Chromium's WebNN service (services/webnn: the context, the graph
 *	builder, the tensors), which the browser runs in its own process as
 *	Chromium does when the GPU process gives no context
 *	(WebNNContextProviderInRenderer). Chromium computes a graph with
 *	LiteRT there; the browser computes it here instead: every operation
 *	of the graph, in the order the page built them, on whole tensors held
 *	as numbers of double precision, each result brought back to its
 *	operand's type (float32, float16, the integers) before the next
 *	operation reads it.
 *
 *	A context computes on the processor whatever device the page asks
 *	for, its tensors are the browser's memory, and its operands are laid
 *	out as NCHW (the filters of conv2d OIHW, of convTranspose2d IOHW).
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "services/webnn/public/cpp/context_properties.h"
#include "services/webnn/public/cpp/data_type_limits.h"
#include "services/webnn/public/cpp/ml_number.h"
#include "services/webnn/public/cpp/operand_descriptor.h"
#include "services/webnn/public/cpp/supported_data_types.h"
#include "services/webnn/public/cpp/supported_tensors.h"
#include "services/webnn/public/cpp/webnn_types.h"
#include "services/webnn/public/mojom/webnn_context_provider.mojom.h"
#include "services/webnn/public/mojom/webnn_graph.mojom.h"
#include "services/webnn/public/mojom/webnn_tensor.mojom.h"
#include "services/webnn/webnn_constant_operand.h"
#include "services/webnn/webnn_context_impl.h"
#include "services/webnn/webnn_context_provider_in_renderer.h"
#include "services/webnn/webnn_graph_impl.h"
#include "services/webnn/webnn_tensor_impl.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/public/platform/scheduler/web_thread_scheduler.h"

/* The services' thread (blob_tessronos.cc) */
void br_on_service_thread( base::OnceClosure task );

namespace webnn {

/* The context the patched provider makes (webnn_context_provider.patch) */
extern WebNNContextImpl::WebNNContextImplPtr (*g_cpu_context_for_renderer)(
    mojo::PendingReceiver<mojom::WebNNContext>,
    base::WeakPtr<WebNNContextProviderInRenderer>,
    mojom::CreateContextOptionsPtr,
    scoped_refptr<base::SingleThreadTaskRunner>,
    scoped_refptr<base::SingleThreadTaskRunner>);

namespace tessronos {

namespace {

using T = OperandDataType;

/* ---------------------------------------------------------------- numbers */

float half_to_float( uint16_t h )
{
	uint32_t s = ( h & 0x8000u ) << 16, e = ( h >> 10 ) & 0x1f, m = h & 0x3ff, f;
	if ( e == 0 ) {
		if ( m == 0 ) f = s;
		else {
			e = 127 - 15 + 1;
			while ( !( m & 0x400 ) ) { m <<= 1; e--; }
			f = s | ( e << 23 ) | ( ( m & 0x3ff ) << 13 );
		}
	} else if ( e == 31 ) f = s | 0x7f800000u | ( m << 13 );
	else f = s | ( ( e + 127 - 15 ) << 23 ) | ( m << 13 );
	float r;
	memcpy(&r, &f, 4);
	return r;
}

uint16_t float_to_half( float v )
{
	uint32_t f;
	memcpy(&f, &v, 4);
	uint32_t s = ( f >> 16 ) & 0x8000, e = ( f >> 23 ) & 0xff, m = f & 0x7fffff;
	if ( e == 0xff ) return (uint16_t)( s | 0x7c00 | ( m ? 0x200 : 0 ) );
	int ne = (int)e - 127 + 15;
	if ( ne >= 31 ) return (uint16_t)( s | 0x7c00 );
	if ( ne <= 0 ) {
		if ( ne < -10 ) return (uint16_t)s;
		m |= 0x800000;
		int shift = 14 - ne;
		uint32_t hm = m >> shift, rem = m & ( ( 1u << shift ) - 1 ), half = 1u << ( shift - 1 );
		if ( rem > half || ( rem == half && ( hm & 1 ) ) ) hm++;
		return (uint16_t)( s | hm );
	}
	uint32_t hm = m >> 13, rem = m & 0x1fff;
	uint32_t r = s | ( (uint32_t)ne << 10 ) | hm;
	if ( rem > 0x1000 || ( rem == 0x1000 && ( hm & 1 ) ) ) r++;
	return (uint16_t)r;
}

size_t type_size( T t )
{
	switch ( t ) {
	case T::kFloat32: case T::kInt32: case T::kUint32: return 4;
	case T::kFloat16: return 2;
	case T::kInt64: case T::kUint64: return 8;
	default: return 1;
	}
}

template <typename I>
double sat( double v )
{
	if ( isnan(v) ) return 0;
	v = trunc(v);
	if ( v < (double)std::numeric_limits<I>::lowest() ) return (double)std::numeric_limits<I>::lowest();
	if ( v > (double)std::numeric_limits<I>::max() ) return (double)std::numeric_limits<I>::max();
	return v;
}

/* A value as its operand's type holds it */
double fit( double v, T t )
{
	switch ( t ) {
	case T::kFloat32: return (double)(float)v;
	case T::kFloat16: return (double)half_to_float(float_to_half((float)v));
	case T::kInt32: return sat<int32_t>(v);
	case T::kUint32: return sat<uint32_t>(v);
	case T::kInt64: return sat<int64_t>(v);
	case T::kUint64: return sat<uint64_t>(v);
	case T::kInt8: return sat<int8_t>(v);
	case T::kUint8: return sat<uint8_t>(v);
	case T::kInt4: return std::clamp(sat<int8_t>(v), -8.0, 7.0);
	case T::kUint4: return std::clamp(sat<uint8_t>(v), 0.0, 15.0);
	}
	return v;
}

std::vector<double> load( const uint8_t *p, size_t n, T t )
{
	std::vector<double> v(n);
	for ( size_t i = 0; i < n; i++ ) {
		switch ( t ) {
		case T::kFloat32: { float x; memcpy(&x, p + i * 4, 4); v[i] = x; break; }
		case T::kFloat16: { uint16_t x; memcpy(&x, p + i * 2, 2); v[i] = half_to_float(x); break; }
		case T::kInt32: { int32_t x; memcpy(&x, p + i * 4, 4); v[i] = x; break; }
		case T::kUint32: { uint32_t x; memcpy(&x, p + i * 4, 4); v[i] = x; break; }
		case T::kInt64: { int64_t x; memcpy(&x, p + i * 8, 8); v[i] = (double)x; break; }
		case T::kUint64: { uint64_t x; memcpy(&x, p + i * 8, 8); v[i] = (double)x; break; }
		case T::kInt8: v[i] = (int8_t)p[i]; break;
		case T::kUint8: v[i] = p[i]; break;
		case T::kInt4: { int x = ( p[i / 2] >> ( ( i & 1 ) * 4 ) ) & 0xf; v[i] = x >= 8 ? x - 16 : x; break; }
		case T::kUint4: v[i] = ( p[i / 2] >> ( ( i & 1 ) * 4 ) ) & 0xf; break;
		}
	}
	return v;
}

void store( const std::vector<double> &v, uint8_t *p, T t )
{
	for ( size_t i = 0; i < v.size(); i++ ) {
		double x = fit(v[i], t);
		switch ( t ) {
		case T::kFloat32: { float y = (float)x; memcpy(p + i * 4, &y, 4); break; }
		case T::kFloat16: { uint16_t y = float_to_half((float)x); memcpy(p + i * 2, &y, 2); break; }
		case T::kInt32: { int32_t y = (int32_t)x; memcpy(p + i * 4, &y, 4); break; }
		case T::kUint32: { uint32_t y = (uint32_t)x; memcpy(p + i * 4, &y, 4); break; }
		case T::kInt64: { int64_t y = (int64_t)x; memcpy(p + i * 8, &y, 8); break; }
		case T::kUint64: { uint64_t y = (uint64_t)x; memcpy(p + i * 8, &y, 8); break; }
		case T::kInt8: p[i] = (uint8_t)(int8_t)x; break;
		case T::kUint8: p[i] = (uint8_t)x; break;
		case T::kInt4: case T::kUint4: {
			uint8_t q = (uint8_t)( (int)x & 0xf );
			if ( i & 1 ) p[i / 2] = (uint8_t)( ( p[i / 2] & 0x0f ) | ( q << 4 ) );
			else p[i / 2] = (uint8_t)( ( p[i / 2] & 0xf0 ) | q );
			break;
		}
		}
	}
}

size_t bytes_of( size_t n, T t )
{
	return ( t == T::kInt4 || t == T::kUint4 ) ? ( n + 1 ) / 2 : n * type_size(t);
}

/* ---------------------------------------------------------------- shapes */

using Shape = std::vector<uint32_t>;

size_t count( const Shape &s )
{
	size_t n = 1;
	for ( uint32_t d : s ) n *= d;
	return n;
}

std::vector<size_t> strides( const Shape &s )
{
	std::vector<size_t> st(s.size());
	size_t k = 1;
	for ( size_t i = s.size(); i-- > 0; ) { st[i] = k; k *= s[i]; }
	return st;
}

/* The flat index of a broadcast operand of shape 'in' at the index 'idx' of 'out' */
size_t bcast( const std::vector<size_t> &idx, const Shape &out, const Shape &in, const std::vector<size_t> &in_st )
{
	size_t f = 0, off = out.size() - in.size();
	for ( size_t d = 0; d < in.size(); d++ ) {
		size_t i = in[d] == 1 ? 0 : idx[d + off];
		f += i * in_st[d];
	}
	return f;
}

void unflat( size_t f, const Shape &s, std::vector<size_t> &idx )
{
	idx.resize(s.size());
	for ( size_t d = s.size(); d-- > 0; ) {
		idx[d] = s[d] ? f % s[d] : 0;
		f = s[d] ? f / s[d] : 0;
	}
}

/* ---------------------------------------------------------------- the graph */

struct Val {
	std::vector<double>	v;
	Shape			shape;
	T			type = T::kFloat32;
};

double sigm( double x ) { return 1.0 / ( 1.0 + exp(-x) ); }

double act( mojom::RecurrentNetworkActivation a, double x )
{
	switch ( a ) {
	case mojom::RecurrentNetworkActivation::kRelu: return x > 0 ? x : 0;
	case mojom::RecurrentNetworkActivation::kSigmoid: return sigm(x);
	case mojom::RecurrentNetworkActivation::kTanh: return tanh(x);
	}
	return x;
}

double round_even( double x )
{
	double r = nearbyint(x);	/* the default rounding: to even */
	return r;
}

class Interpreter {
public:
	Interpreter( const mojom::GraphInfo &g, std::vector<Val> &vals ) : g_(g), vals_(vals) {}

	bool Run( std::string *err )
	{
		for ( const auto &op : g_.operations ) {
			if ( !One(*op) ) {
				*err = "operation " + std::to_string((int)op->which()) + " is not computed";
				return false;
			}
		}
		return true;
	}

private:
	Val &V( OperandId id ) { return vals_[id.value()]; }

	/* The result of an operation: its operand's shape and type, the values brought to the type */
	void Put( OperandId id, std::vector<double> v )
	{
		Val &o = V(id);
		const auto &d = g_.operands[id.value()]->descriptor;
		o.type = d.data_type();
		o.shape = Shape(d.shape().begin(), d.shape().end());
		for ( double &x : v ) x = fit(x, o.type);
		o.v = std::move(v);
	}

	const Shape &OutShape( OperandId id )
	{
		static thread_local Shape s;
		const auto &d = g_.operands[id.value()]->descriptor;
		s.assign(d.shape().begin(), d.shape().end());
		return s;
	}

	T OutType( OperandId id ) { return g_.operands[id.value()]->descriptor.data_type(); }

	bool One( const mojom::Operation &op );

	void Binary( const mojom::ElementWiseBinary &b );
	void Unary( const mojom::ElementWiseUnary &u );
	void Map( OperandId in, OperandId out, double ( *f )( double, const void * ), const void *arg );
	void Conv( const mojom::Conv2d &c );
	void Pool( const mojom::Pool2d &p );
	void Reduce( const mojom::Reduce &r );
	void Gemm( const mojom::Gemm &g );
	void Matmul( const mojom::Matmul &m );
	void GruCell( const std::vector<double> &x, size_t batch, size_t isz, const double *W, const double *R,
		      const double *B, const double *RB, std::vector<double> &h, size_t hs, bool reset_after,
		      mojom::GruWeightLayout layout, const std::vector<mojom::RecurrentNetworkActivation> &a );
	void LstmCell( const std::vector<double> &x, size_t batch, size_t isz, const double *W, const double *R,
		       const double *B, const double *RB, const double *P, std::vector<double> &h, std::vector<double> &c,
		       size_t hs, mojom::LstmWeightLayout layout, const std::vector<mojom::RecurrentNetworkActivation> &a );

	const mojom::GraphInfo	&g_;
	std::vector<Val>	&vals_;
};

void Interpreter::Map( OperandId in, OperandId out, double ( *f )( double, const void * ), const void *arg )
{
	const Val &a = V(in);
	std::vector<double> r(a.v.size());
	for ( size_t i = 0; i < r.size(); i++ ) r[i] = f(a.v[i], arg);
	Put(out, std::move(r));
}

void Interpreter::Binary( const mojom::ElementWiseBinary &b )
{
	using K = mojom::ElementWiseBinary::Kind;
	const Val &x = V(b.lhs_operand_id), &y = V(b.rhs_operand_id);
	Shape out = OutShape(b.output_operand_id);
	auto xs = strides(x.shape), ys = strides(y.shape);
	bool ints = !( x.type == T::kFloat32 || x.type == T::kFloat16 );
	size_t n = count(out);
	std::vector<double> r(n);
	std::vector<size_t> idx;
	for ( size_t i = 0; i < n; i++ ) {
		unflat(i, out, idx);
		double a = x.v[bcast(idx, out, x.shape, xs)], c = y.v[bcast(idx, out, y.shape, ys)];
		double v = 0;
		switch ( b.kind ) {
		case K::kAdd: v = a + c; break;
		case K::kSub: v = a - c; break;
		case K::kMul: v = a * c; break;
		case K::kDiv: v = ints ? ( c == 0 ? 0 : trunc(a / c) ) : a / c; break;
		case K::kMax: v = std::max(a, c); break;
		case K::kMin: v = std::min(a, c); break;
		case K::kPow: v = pow(a, c); break;
		case K::kEqual: v = a == c; break;
		case K::kGreater: v = a > c; break;
		case K::kGreaterOrEqual: v = a >= c; break;
		case K::kLesser: v = a < c; break;
		case K::kLesserOrEqual: v = a <= c; break;
		case K::kNotEqual: v = a != c; break;
		case K::kLogicalAnd: v = ( a != 0 ) && ( c != 0 ); break;
		case K::kLogicalOr: v = ( a != 0 ) || ( c != 0 ); break;
		case K::kLogicalXor: v = ( a != 0 ) != ( c != 0 ); break;
		}
		r[i] = v;
	}
	Put(b.output_operand_id, std::move(r));
}

void Interpreter::Unary( const mojom::ElementWiseUnary &u )
{
	using K = mojom::ElementWiseUnary::Kind;
	const Val &a = V(u.input_operand_id);
	std::vector<double> r(a.v.size());
	for ( size_t i = 0; i < r.size(); i++ ) {
		double x = a.v[i], v = x;
		switch ( u.kind ) {
		case K::kAbs: v = fabs(x); break;
		case K::kCeil: v = ceil(x); break;
		case K::kCos: v = cos(x); break;
		case K::kExp: v = exp(x); break;
		case K::kFloor: v = floor(x); break;
		case K::kLog: v = log(x); break;
		case K::kNeg: v = -x; break;
		case K::kSign: v = x > 0 ? 1 : x < 0 ? -1 : 0; break;
		case K::kRoundEven: v = round_even(x); break;
		case K::kSin: v = sin(x); break;
		case K::kTan: v = tan(x); break;
		case K::kIsNaN: v = isnan(x); break;
		case K::kIsInfinite: v = isinf(x); break;
		case K::kLogicalNot: v = x == 0; break;
		case K::kIdentity: v = x; break;
		case K::kSqrt: v = sqrt(x); break;
		case K::kErf: v = erf(x); break;
		case K::kReciprocal: v = 1.0 / x; break;
		case K::kCast: v = x; break;
		}
		r[i] = v;
	}
	Put(u.output_operand_id, std::move(r));
}

/* conv2d (NCHW, OIHW) and convTranspose2d (NCHW, IOHW) */
void Interpreter::Conv( const mojom::Conv2d &c )
{
	const Val &in = V(c.input_operand_id), &w = V(c.filter_operand_id);
	Shape out = OutShape(c.output_operand_id);
	size_t N = in.shape[0], C = in.shape[1], H = in.shape[2], W = in.shape[3];
	size_t OC = out[1], OH = out[2], OW = out[3], G = c.groups ? c.groups : 1;
	size_t KH = w.shape[2], KW = w.shape[3];
	long pt = c.padding->beginning->height, pl = c.padding->beginning->width;
	size_t sh = c.strides->height, sw = c.strides->width, dh = c.dilations->height, dw = c.dilations->width;
	std::vector<double> r(count(out), 0.0);
	const Val *bias = c.bias_operand_id ? &V(*c.bias_operand_id) : nullptr;
	if ( c.kind == mojom::Conv2d::Kind::kDirect ) {
		size_t icg = C / G, ocg = OC / G;
		for ( size_t n = 0; n < N; n++ )
		for ( size_t oc = 0; oc < OC; oc++ ) {
			size_t g = oc / ocg;
			for ( size_t oy = 0; oy < OH; oy++ )
			for ( size_t ox = 0; ox < OW; ox++ ) {
				double s = bias ? bias->v[oc] : 0;
				for ( size_t ic = 0; ic < icg; ic++ )
				for ( size_t ky = 0; ky < KH; ky++ ) {
					long iy = (long)( oy * sh + ky * dh ) - pt;
					if ( iy < 0 || iy >= (long)H ) continue;
					for ( size_t kx = 0; kx < KW; kx++ ) {
						long ix = (long)( ox * sw + kx * dw ) - pl;
						if ( ix < 0 || ix >= (long)W ) continue;
						s += in.v[( ( n * C + g * icg + ic ) * H + iy ) * W + ix]
						     * w.v[( ( oc * icg + ic ) * KH + ky ) * KW + kx];
					}
				}
				r[( ( n * OC + oc ) * OH + oy ) * OW + ox] = s;
			}
		}
	} else {
		size_t icg = C / G, ocg = OC / G;
		for ( size_t n = 0; n < N; n++ )
		for ( size_t ic = 0; ic < C; ic++ ) {
			size_t g = ic / icg;
			for ( size_t iy = 0; iy < H; iy++ )
			for ( size_t ix = 0; ix < W; ix++ ) {
				double x = in.v[( ( n * C + ic ) * H + iy ) * W + ix];
				for ( size_t o = 0; o < ocg; o++ )
				for ( size_t ky = 0; ky < KH; ky++ ) {
					long oy = (long)( iy * sh + ky * dh ) - pt;
					if ( oy < 0 || oy >= (long)OH ) continue;
					for ( size_t kx = 0; kx < KW; kx++ ) {
						long ox = (long)( ix * sw + kx * dw ) - pl;
						if ( ox < 0 || ox >= (long)OW ) continue;
						size_t oc = g * ocg + o;
						r[( ( n * OC + oc ) * OH + oy ) * OW + ox] +=
							x * w.v[( ( ic * ocg + o ) * KH + ky ) * KW + kx];
					}
				}
			}
		}
		if ( bias ) {
			for ( size_t i = 0; i < r.size(); i++ ) r[i] += bias->v[( i / ( OH * OW ) ) % OC];
		}
	}
	Put(c.output_operand_id, std::move(r));
}

void Interpreter::Pool( const mojom::Pool2d &p )
{
	using K = mojom::Pool2d::Kind;
	const Val &in = V(p.input_operand_id);
	Shape out = OutShape(p.output_operand_id);
	size_t N = in.shape[0], C = in.shape[1], H = in.shape[2], W = in.shape[3], OH = out[2], OW = out[3];
	size_t KH = p.window_dimensions->height, KW = p.window_dimensions->width;
	long pt = p.padding->beginning->height, pl = p.padding->beginning->width;
	size_t sh = p.strides->height, sw = p.strides->width, dh = p.dilations->height, dw = p.dilations->width;
	std::vector<double> r(count(out));
	for ( size_t n = 0; n < N; n++ )
	for ( size_t c = 0; c < C; c++ )
	for ( size_t oy = 0; oy < OH; oy++ )
	for ( size_t ox = 0; ox < OW; ox++ ) {
		double s = p.kind == K::kMaxPool2d ? -INFINITY : 0;
		size_t k = 0;
		for ( size_t ky = 0; ky < KH; ky++ ) {
			long iy = (long)( oy * sh + ky * dh ) - pt;
			if ( iy < 0 || iy >= (long)H ) continue;
			for ( size_t kx = 0; kx < KW; kx++ ) {
				long ix = (long)( ox * sw + kx * dw ) - pl;
				if ( ix < 0 || ix >= (long)W ) continue;
				double x = in.v[( ( n * C + c ) * H + iy ) * W + ix];
				if ( p.kind == K::kMaxPool2d ) s = std::max(s, x);
				else if ( p.kind == K::kL2Pool2d ) s += x * x;
				else s += x;
				k++;
			}
		}
		if ( p.kind == K::kAveragePool2d ) s = k ? s / k : 0;
		if ( p.kind == K::kL2Pool2d ) s = sqrt(s);
		r[( ( n * C + c ) * OH + oy ) * OW + ox] = s;
	}
	Put(p.output_operand_id, std::move(r));
}

void Interpreter::Reduce( const mojom::Reduce &rd )
{
	using K = mojom::Reduce::Kind;
	const Val &in = V(rd.input_operand_id);
	std::vector<bool> red(in.shape.size(), false);
	for ( uint32_t a : rd.axes ) red[a] = true;
	Shape kept;
	for ( size_t d = 0; d < in.shape.size(); d++ ) kept.push_back(red[d] ? 1 : in.shape[d]);
	size_t n = count(kept);
	std::vector<double> acc(n, 0.0), cnt(n, 0.0);
	if ( rd.kind == K::kMax ) std::fill(acc.begin(), acc.end(), -INFINITY);
	if ( rd.kind == K::kMin ) std::fill(acc.begin(), acc.end(), INFINITY);
	if ( rd.kind == K::kProduct ) std::fill(acc.begin(), acc.end(), 1.0);
	auto ks = strides(kept);
	std::vector<size_t> idx;
	for ( size_t i = 0; i < in.v.size(); i++ ) {
		unflat(i, in.shape, idx);
		size_t f = 0;
		for ( size_t d = 0; d < idx.size(); d++ ) f += ( red[d] ? 0 : idx[d] ) * ks[d];
		double x = in.v[i];
		switch ( rd.kind ) {
		case K::kL1: acc[f] += fabs(x); break;
		case K::kL2: case K::kSumSquare: acc[f] += x * x; break;
		case K::kLogSum: case K::kSum: case K::kMean: acc[f] += x; break;
		case K::kLogSumExp: acc[f] += exp(x); break;
		case K::kMax: acc[f] = std::max(acc[f], x); break;
		case K::kMin: acc[f] = std::min(acc[f], x); break;
		case K::kProduct: acc[f] *= x; break;
		}
		cnt[f] += 1;
	}
	for ( size_t i = 0; i < n; i++ ) {
		switch ( rd.kind ) {
		case K::kL2: acc[i] = sqrt(acc[i]); break;
		case K::kLogSum: case K::kLogSumExp: acc[i] = log(acc[i]); break;
		case K::kMean: acc[i] = cnt[i] ? acc[i] / cnt[i] : NAN; break;
		default: break;
		}
	}
	Put(rd.output_operand_id, std::move(acc));
}

void Interpreter::Gemm( const mojom::Gemm &g )
{
	const Val &a = V(g.a_operand_id), &b = V(g.b_operand_id);
	size_t M = g.a_transpose ? a.shape[1] : a.shape[0], K = g.a_transpose ? a.shape[0] : a.shape[1];
	size_t N = g.b_transpose ? b.shape[0] : b.shape[1];
	Shape out = {(uint32_t)M, (uint32_t)N};
	std::vector<double> r(M * N);
	const Val *c = g.c_operand_id ? &V(*g.c_operand_id) : nullptr;
	auto cs = c ? strides(c->shape) : std::vector<size_t>();
	std::vector<size_t> idx(2);
	for ( size_t i = 0; i < M; i++ )
	for ( size_t j = 0; j < N; j++ ) {
		double s = 0;
		for ( size_t k = 0; k < K; k++ ) {
			double x = g.a_transpose ? a.v[k * M + i] : a.v[i * K + k];
			double y = g.b_transpose ? b.v[j * K + k] : b.v[k * N + j];
			s += x * y;
		}
		s *= g.alpha;
		if ( c ) {
			idx[0] = i; idx[1] = j;
			s += g.beta * c->v[bcast(idx, out, c->shape, cs)];
		}
		r[i * N + j] = s;
	}
	Put(g.output_operand_id, std::move(r));
}

void Interpreter::Matmul( const mojom::Matmul &m )
{
	const Val &a = V(m.a_operand_id), &b = V(m.b_operand_id);
	Shape out = OutShape(m.output_operand_id);
	size_t R = out.size(), M = out[R - 2], N = out[R - 1], K = a.shape[a.shape.size() - 1];
	Shape batch(out.begin(), out.end() - 2), ab(a.shape.begin(), a.shape.end() - 2), bb(b.shape.begin(), b.shape.end() - 2);
	auto as = strides(ab), bs = strides(bb);
	size_t nb = count(batch);
	std::vector<double> r(count(out));
	std::vector<size_t> idx;
	for ( size_t q = 0; q < nb; q++ ) {
		unflat(q, batch, idx);
		size_t ao = ab.empty() ? 0 : bcast(idx, batch, ab, as), bo = bb.empty() ? 0 : bcast(idx, batch, bb, bs);
		const double *pa = &a.v[ao * M * K], *pb = &b.v[bo * K * N];
		double *pr = &r[q * M * N];
		for ( size_t i = 0; i < M; i++ )
		for ( size_t j = 0; j < N; j++ ) {
			double s = 0;
			for ( size_t k = 0; k < K; k++ ) s += pa[i * K + k] * pb[k * N + j];
			pr[i * N + j] = s;
		}
	}
	Put(m.output_operand_id, std::move(r));
}

/* One step of a GRU: h (batch × hs) becomes the next hidden state */
void Interpreter::GruCell( const std::vector<double> &x, size_t batch, size_t isz, const double *W, const double *R,
			   const double *B, const double *RB, std::vector<double> &h, size_t hs, bool reset_after,
			   mojom::GruWeightLayout layout, const std::vector<mojom::RecurrentNetworkActivation> &a )
{
	/* the gates' rows in the weights: update z, reset r, new n */
	size_t zi, ri, ni;
	if ( layout == mojom::GruWeightLayout::kZrn ) { zi = 0; ri = 1; ni = 2; }
	else { ri = 0; zi = 1; ni = 2; }
	std::vector<double> nh(batch * hs);
	for ( size_t b = 0; b < batch; b++ )
	for ( size_t j = 0; j < hs; j++ ) {
		auto xw = [&]( size_t gate ) {
			double s = B ? B[gate * hs + j] : 0;
			for ( size_t k = 0; k < isz; k++ ) s += x[b * isz + k] * W[( gate * hs + j ) * isz + k];
			return s;
		};
		auto hr = [&]( size_t gate, const double *hv ) {
			double s = RB ? RB[gate * hs + j] : 0;
			for ( size_t k = 0; k < hs; k++ ) s += hv[k] * R[( gate * hs + j ) * hs + k];
			return s;
		};
		const double *hb = &h[b * hs];
		double z = act(a[0], xw(zi) + hr(zi, hb));
		double n;
		if ( reset_after ) {
			double rr = act(a[0], xw(ri) + hr(ri, hb));
			n = act(a[1], xw(ni) + rr * hr(ni, hb));
		} else {
			std::vector<double> rh(hs);
			for ( size_t k = 0; k < hs; k++ ) {
				double rk = B ? B[ri * hs + k] : 0;
				for ( size_t m = 0; m < isz; m++ ) rk += x[b * isz + m] * W[( ri * hs + k ) * isz + m];
				double rr = RB ? RB[ri * hs + k] : 0;
				for ( size_t m = 0; m < hs; m++ ) rr += hb[m] * R[( ri * hs + k ) * hs + m];
				rh[k] = act(a[0], rk + rr) * hb[k];
			}
			n = act(a[1], xw(ni) + hr(ni, rh.data()));
		}
		nh[b * hs + j] = ( 1 - z ) * n + z * hb[j];
	}
	h = std::move(nh);
}

/* One step of an LSTM: h and c (batch × hs) become the next states */
void Interpreter::LstmCell( const std::vector<double> &x, size_t batch, size_t isz, const double *W, const double *R,
			    const double *B, const double *RB, const double *P, std::vector<double> &h, std::vector<double> &c,
			    size_t hs, mojom::LstmWeightLayout layout, const std::vector<mojom::RecurrentNetworkActivation> &a )
{
	/* the gates' rows in the weights: input i, output o, forget f, cell g */
	size_t ii, oi, fi, gi;
	if ( layout == mojom::LstmWeightLayout::kIofg ) { ii = 0; oi = 1; fi = 2; gi = 3; }
	else { ii = 0; fi = 1; gi = 2; oi = 3; }
	std::vector<double> nh(batch * hs), nc(batch * hs);
	for ( size_t b = 0; b < batch; b++ )
	for ( size_t j = 0; j < hs; j++ ) {
		auto pre = [&]( size_t gate ) {
			double s = ( B ? B[gate * hs + j] : 0 ) + ( RB ? RB[gate * hs + j] : 0 );
			for ( size_t k = 0; k < isz; k++ ) s += x[b * isz + k] * W[( gate * hs + j ) * isz + k];
			for ( size_t k = 0; k < hs; k++ ) s += h[b * hs + k] * R[( gate * hs + j ) * hs + k];
			return s;
		};
		double cp = c[b * hs + j];
		/* the peephole's weights are of the input, output and forget gates, in that order */
		double ig = act(a[0], pre(ii) + ( P ? P[0 * hs + j] * cp : 0 ));
		double fg = act(a[0], pre(fi) + ( P ? P[2 * hs + j] * cp : 0 ));
		double gg = act(a[1], pre(gi));
		double cn = fg * cp + ig * gg;
		double og = act(a[0], pre(oi) + ( P ? P[1 * hs + j] * cn : 0 ));
		nc[b * hs + j] = cn;
		nh[b * hs + j] = og * act(a[2], cn);
	}
	h = std::move(nh);
	c = std::move(nc);
}

double f_relu( double x, const void * ) { return x > 0 ? x : 0; }
double f_sigmoid( double x, const void * ) { return sigm(x); }
double f_tanh( double x, const void * ) { return tanh(x); }
double f_softplus( double x, const void * ) { return x > 30 ? x : log1p(exp(x)); }
double f_softsign( double x, const void * ) { return x / ( 1 + fabs(x) ); }
double f_gelu( double x, const void * ) { return 0.5 * x * ( 1 + erf(x / sqrt(2.0)) ); }
double f_hardswish( double x, const void * ) { return x * std::max(0.0, std::min(6.0, x + 3)) / 6; }
double f_elu( double x, const void *a ) { return x < 0 ? *(const float *)a * ( exp(x) - 1 ) : x; }
double f_leaky( double x, const void *a ) { return x < 0 ? *(const float *)a * x : x; }
struct AB { double a, b; };
double f_linear( double x, const void *p ) { auto *q = (const AB *)p; return q->a * x + q->b; }
double f_hardsig( double x, const void *p ) { auto *q = (const AB *)p; return std::max(0.0, std::min(1.0, q->a * x + q->b)); }
double f_clamp( double x, const void *p ) { auto *q = (const AB *)p; return std::min(std::max(x, q->a), q->b); }

bool Interpreter::One( const mojom::Operation &op )
{
	using Tag = mojom::Operation::Tag;
	std::vector<size_t> idx, jdx;
	switch ( op.which() ) {
	case Tag::kElementWiseBinary: Binary(*op.get_element_wise_binary()); return true;
	case Tag::kElementWiseUnary: Unary(*op.get_element_wise_unary()); return true;
	case Tag::kRelu: Map(op.get_relu()->input_operand_id, op.get_relu()->output_operand_id, f_relu, nullptr); return true;
	case Tag::kSigmoid: Map(op.get_sigmoid()->input_operand_id, op.get_sigmoid()->output_operand_id, f_sigmoid, nullptr); return true;
	case Tag::kTanh: Map(op.get_tanh()->input_operand_id, op.get_tanh()->output_operand_id, f_tanh, nullptr); return true;
	case Tag::kSoftplus: Map(op.get_softplus()->input_operand_id, op.get_softplus()->output_operand_id, f_softplus, nullptr); return true;
	case Tag::kSoftsign: Map(op.get_softsign()->input_operand_id, op.get_softsign()->output_operand_id, f_softsign, nullptr); return true;
	case Tag::kGelu: Map(op.get_gelu()->input_operand_id, op.get_gelu()->output_operand_id, f_gelu, nullptr); return true;
	case Tag::kHardSwish: Map(op.get_hard_swish()->input_operand_id, op.get_hard_swish()->output_operand_id, f_hardswish, nullptr); return true;
	case Tag::kElu: { const auto &e = *op.get_elu(); Map(e.input_operand_id, e.output_operand_id, f_elu, &e.alpha); return true; }
	case Tag::kLeakyRelu: { const auto &e = *op.get_leaky_relu(); Map(e.input_operand_id, e.output_operand_id, f_leaky, &e.alpha); return true; }
	case Tag::kLinear: { const auto &e = *op.get_linear(); AB p{e.alpha, e.beta}; Map(e.input_operand_id, e.output_operand_id, f_linear, &p); return true; }
	case Tag::kHardSigmoid: { const auto &e = *op.get_hard_sigmoid(); AB p{e.alpha, e.beta}; Map(e.input_operand_id, e.output_operand_id, f_hardsig, &p); return true; }
	case Tag::kClamp: {
		const auto &e = *op.get_clamp();
		AB p{e.min_value.AsFloat64(), e.max_value.AsFloat64()};
		Map(e.input_operand_id, e.output_operand_id, f_clamp, &p);
		return true;
	}
	case Tag::kConv2d: Conv(*op.get_conv2d()); return true;
	case Tag::kPool2d: Pool(*op.get_pool2d()); return true;
	case Tag::kReduce: Reduce(*op.get_reduce()); return true;
	case Tag::kGemm: Gemm(*op.get_gemm()); return true;
	case Tag::kMatmul: Matmul(*op.get_matmul()); return true;
	case Tag::kReshape: { const auto &e = *op.get_reshape(); Put(e.output_operand_id, V(e.input_operand_id).v); return true; }
	case Tag::kExpand: {
		const auto &e = *op.get_expand();
		const Val &a = V(e.input_operand_id);
		Shape out = OutShape(e.output_operand_id);
		auto as = strides(a.shape);
		std::vector<double> r(count(out));
		for ( size_t i = 0; i < r.size(); i++ ) { unflat(i, out, idx); r[i] = a.v[bcast(idx, out, a.shape, as)]; }
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kTranspose: {
		const auto &e = *op.get_transpose();
		const Val &a = V(e.input_operand_id);
		Shape out = OutShape(e.output_operand_id);
		auto as = strides(a.shape);
		std::vector<double> r(count(out));
		for ( size_t i = 0; i < r.size(); i++ ) {
			unflat(i, out, idx);
			size_t f = 0;
			for ( size_t d = 0; d < idx.size(); d++ ) f += idx[d] * as[e.permutation[d]];
			r[i] = a.v[f];
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kConcat: {
		const auto &e = *op.get_concat();
		Shape out = OutShape(e.output_operand_id);
		std::vector<double> r(count(out));
		auto os = strides(out);
		size_t at = 0;
		for ( OperandId id : e.input_operand_ids ) {
			const Val &a = V(id);
			for ( size_t i = 0; i < a.v.size(); i++ ) {
				unflat(i, a.shape, idx);
				idx[e.axis] += at;
				size_t f = 0;
				for ( size_t d = 0; d < idx.size(); d++ ) f += idx[d] * os[d];
				r[f] = a.v[i];
			}
			at += a.shape[e.axis];
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kSplit: {
		const auto &e = *op.get_split();
		const Val &a = V(e.input_operand_id);
		auto as = strides(a.shape);
		size_t at = 0;
		for ( OperandId id : e.output_operand_ids ) {
			Shape out = OutShape(id);
			std::vector<double> r(count(out));
			for ( size_t i = 0; i < r.size(); i++ ) {
				unflat(i, out, idx);
				idx[e.axis] += at;
				size_t f = 0;
				for ( size_t d = 0; d < idx.size(); d++ ) f += idx[d] * as[d];
				r[i] = a.v[f];
			}
			at += out[e.axis];
			Put(id, std::move(r));
		}
		return true;
	}
	case Tag::kSlice: {
		const auto &e = *op.get_slice();
		const Val &a = V(e.input_operand_id);
		Shape out = OutShape(e.output_operand_id);
		auto as = strides(a.shape);
		std::vector<double> r(count(out));
		for ( size_t i = 0; i < r.size(); i++ ) {
			unflat(i, out, idx);
			size_t f = 0;
			for ( size_t d = 0; d < idx.size(); d++ )
				f += ( e.ranges[d].start + idx[d] * e.ranges[d].stride ) * as[d];
			r[i] = a.v[f];
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kPad: {
		const auto &e = *op.get_pad();
		const Val &a = V(e.input_operand_id);
		Shape out = OutShape(e.output_operand_id);
		auto as = strides(a.shape);
		double cv = e.mode->is_constant() ? e.mode->get_constant()->value.AsFloat64() : 0;
		std::vector<double> r(count(out));
		for ( size_t i = 0; i < r.size(); i++ ) {
			unflat(i, out, idx);
			bool outside = false;
			size_t f = 0;
			for ( size_t d = 0; d < idx.size(); d++ ) {
				long p = (long)idx[d] - (long)e.beginning_padding[d], n = a.shape[d];
				if ( p < 0 || p >= n ) {
					if ( e.mode->is_constant() ) { outside = true; break; }
					if ( e.mode->is_edge() ) p = p < 0 ? 0 : n - 1;
					else {	/* reflection */
						while ( p < 0 || p >= n ) p = p < 0 ? -p : 2 * ( n - 1 ) - p;
					}
				}
				f += (size_t)p * as[d];
			}
			r[i] = outside ? cv : a.v[f];
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kReverse: {
		const auto &e = *op.get_reverse();
		const Val &a = V(e.input_operand_id);
		auto as = strides(a.shape);
		std::vector<bool> rev(a.shape.size(), false);
		for ( uint32_t x : e.axes ) rev[x] = true;
		std::vector<double> r(a.v.size());
		for ( size_t i = 0; i < r.size(); i++ ) {
			unflat(i, a.shape, idx);
			size_t f = 0;
			for ( size_t d = 0; d < idx.size(); d++ ) f += ( rev[d] ? a.shape[d] - 1 - idx[d] : idx[d] ) * as[d];
			r[i] = a.v[f];
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kTile: {
		const auto &e = *op.get_tile();
		const Val &a = V(e.input_operand_id);
		Shape out = OutShape(e.output_operand_id);
		auto as = strides(a.shape);
		std::vector<double> r(count(out));
		for ( size_t i = 0; i < r.size(); i++ ) {
			unflat(i, out, idx);
			size_t f = 0;
			for ( size_t d = 0; d < idx.size(); d++ ) f += ( idx[d] % a.shape[d] ) * as[d];
			r[i] = a.v[f];
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kTriangular: {
		const auto &e = *op.get_triangular();
		const Val &a = V(e.input_operand_id);
		std::vector<double> r(a.v);
		size_t R = a.shape.size();
		size_t rows = R >= 2 ? a.shape[R - 2] : 1, cols = R >= 1 ? a.shape[R - 1] : 1;
		for ( size_t i = 0; i < r.size(); i++ ) {
			long row = (long)( ( i / cols ) % rows ), col = (long)( i % cols );
			bool keep = e.upper ? col - row >= e.diagonal : col - row <= e.diagonal;
			if ( !keep ) r[i] = 0;
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kWhere: {
		const auto &e = *op.get_where();
		const Val &c = V(e.condition_operand_id), &t = V(e.true_value_operand_id), &f = V(e.false_value_operand_id);
		Shape out = OutShape(e.output_operand_id);
		auto cs = strides(c.shape), ts = strides(t.shape), fs = strides(f.shape);
		std::vector<double> r(count(out));
		for ( size_t i = 0; i < r.size(); i++ ) {
			unflat(i, out, idx);
			r[i] = c.v[bcast(idx, out, c.shape, cs)] != 0 ? t.v[bcast(idx, out, t.shape, ts)] : f.v[bcast(idx, out, f.shape, fs)];
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kPrelu: {
		const auto &e = *op.get_prelu();
		const Val &a = V(e.input_operand_id), &s = V(e.slope_operand_id);
		Shape out = OutShape(e.output_operand_id);
		auto as = strides(a.shape), ss = strides(s.shape);
		std::vector<double> r(count(out));
		for ( size_t i = 0; i < r.size(); i++ ) {
			unflat(i, out, idx);
			double x = a.v[bcast(idx, out, a.shape, as)];
			r[i] = x < 0 ? x * s.v[bcast(idx, out, s.shape, ss)] : x;
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kSoftmax: {
		const auto &e = *op.get_softmax();
		const Val &a = V(e.input_operand_id);
		size_t ax = e.axis, n = a.shape[ax], inner = 1, outer = 1;
		for ( size_t d = ax + 1; d < a.shape.size(); d++ ) inner *= a.shape[d];
		for ( size_t d = 0; d < ax; d++ ) outer *= a.shape[d];
		std::vector<double> r(a.v.size());
		for ( size_t o = 0; o < outer; o++ )
		for ( size_t in = 0; in < inner; in++ ) {
			double mx = -INFINITY, s = 0;
			for ( size_t k = 0; k < n; k++ ) mx = std::max(mx, a.v[( o * n + k ) * inner + in]);
			for ( size_t k = 0; k < n; k++ ) s += exp(a.v[( o * n + k ) * inner + in] - mx);
			for ( size_t k = 0; k < n; k++ ) r[( o * n + k ) * inner + in] = exp(a.v[( o * n + k ) * inner + in] - mx) / s;
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kArgMinMax: {
		const auto &e = *op.get_arg_min_max();
		const Val &a = V(e.input_operand_id);
		size_t ax = e.axis, n = a.shape[ax], inner = 1, outer = 1;
		for ( size_t d = ax + 1; d < a.shape.size(); d++ ) inner *= a.shape[d];
		for ( size_t d = 0; d < ax; d++ ) outer *= a.shape[d];
		std::vector<double> r(outer * inner);
		for ( size_t o = 0; o < outer; o++ )
		for ( size_t in = 0; in < inner; in++ ) {
			size_t best = 0;
			for ( size_t k = 1; k < n; k++ ) {
				double x = a.v[( o * n + k ) * inner + in], y = a.v[( o * n + best ) * inner + in];
				if ( e.kind == mojom::ArgMinMax::Kind::kMax ? x > y : x < y ) best = k;
			}
			r[o * inner + in] = (double)best;
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kCumulativeSum: {
		const auto &e = *op.get_cumulative_sum();
		const Val &a = V(e.input_operand_id);
		size_t ax = e.axis, n = a.shape[ax], inner = 1, outer = 1;
		for ( size_t d = ax + 1; d < a.shape.size(); d++ ) inner *= a.shape[d];
		for ( size_t d = 0; d < ax; d++ ) outer *= a.shape[d];
		std::vector<double> r(a.v.size());
		for ( size_t o = 0; o < outer; o++ )
		for ( size_t in = 0; in < inner; in++ ) {
			double s = 0;
			for ( size_t t = 0; t < n; t++ ) {
				size_t k = e.reversed ? n - 1 - t : t, f = ( o * n + k ) * inner + in;
				if ( e.exclusive ) { r[f] = s; s += a.v[f]; }
				else { s += a.v[f]; r[f] = s; }
			}
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kBatchNormalization: {
		const auto &e = *op.get_batch_normalization();
		const Val &a = V(e.input_operand_id), &mean = V(e.mean_operand_id), &var = V(e.variance_operand_id);
		const Val *sc = e.scale_operand_id ? &V(*e.scale_operand_id) : nullptr;
		const Val *bi = e.bias_operand_id ? &V(*e.bias_operand_id) : nullptr;
		size_t ax = e.axis, inner = 1;
		for ( size_t d = ax + 1; d < a.shape.size(); d++ ) inner *= a.shape[d];
		std::vector<double> r(a.v.size());
		for ( size_t i = 0; i < r.size(); i++ ) {
			size_t c = ( i / inner ) % a.shape[ax];
			double y = ( a.v[i] - mean.v[c] ) / sqrt(var.v[c] + e.epsilon);
			if ( sc ) y *= sc->v[c];
			if ( bi ) y += bi->v[c];
			r[i] = y;
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kInstanceNormalization: {
		const auto &e = *op.get_instance_normalization();
		const Val &a = V(e.input_operand_id);
		const Val *sc = e.scale_operand_id ? &V(*e.scale_operand_id) : nullptr;
		const Val *bi = e.bias_operand_id ? &V(*e.bias_operand_id) : nullptr;
		size_t N = a.shape[0], C = a.shape[1], HW = a.v.size() / ( N * C );
		std::vector<double> r(a.v.size());
		for ( size_t n = 0; n < N; n++ )
		for ( size_t c = 0; c < C; c++ ) {
			const double *p = &a.v[( n * C + c ) * HW];
			double m = 0, v = 0;
			for ( size_t k = 0; k < HW; k++ ) m += p[k];
			m /= HW;
			for ( size_t k = 0; k < HW; k++ ) v += ( p[k] - m ) * ( p[k] - m );
			v /= HW;
			for ( size_t k = 0; k < HW; k++ ) {
				double y = ( p[k] - m ) / sqrt(v + e.epsilon);
				if ( sc ) y *= sc->v[c];
				if ( bi ) y += bi->v[c];
				r[( n * C + c ) * HW + k] = y;
			}
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kLayerNormalization: {
		const auto &e = *op.get_layer_normalization();
		const Val &a = V(e.input_operand_id);
		const Val *sc = e.scale_operand_id ? &V(*e.scale_operand_id) : nullptr;
		const Val *bi = e.bias_operand_id ? &V(*e.bias_operand_id) : nullptr;
		std::vector<bool> red(a.shape.size(), false);
		Shape ns;			/* the shape of scale and bias: the axes', in their order */
		for ( uint32_t x : e.axes ) { red[x] = true; ns.push_back(a.shape[x]); }
		Shape kept;
		for ( size_t d = 0; d < a.shape.size(); d++ ) kept.push_back(red[d] ? 1 : a.shape[d]);
		auto ks = strides(kept), nst = strides(ns);
		std::vector<double> sum(count(kept), 0.0), sq(count(kept), 0.0);
		size_t per = count(ns);
		std::vector<size_t> where(a.v.size());
		for ( size_t i = 0; i < a.v.size(); i++ ) {
			unflat(i, a.shape, idx);
			size_t f = 0;
			for ( size_t d = 0; d < idx.size(); d++ ) f += ( red[d] ? 0 : idx[d] ) * ks[d];
			where[i] = f;
			sum[f] += a.v[i];
		}
		for ( double &s : sum ) s /= per ? per : 1;
		for ( size_t i = 0; i < a.v.size(); i++ ) { double dd = a.v[i] - sum[where[i]]; sq[where[i]] += dd * dd; }
		for ( double &s : sq ) s /= per ? per : 1;
		std::vector<double> r(a.v.size());
		for ( size_t i = 0; i < a.v.size(); i++ ) {
			double y = ( a.v[i] - sum[where[i]] ) / sqrt(sq[where[i]] + e.epsilon);
			if ( sc || bi ) {
				unflat(i, a.shape, idx);
				size_t f = 0;
				for ( size_t k = 0; k < e.axes.size(); k++ ) f += idx[e.axes[k]] * nst[k];
				if ( sc ) y *= sc->v[f];
				if ( bi ) y += bi->v[f];
			}
			r[i] = y;
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kGather: {
		const auto &e = *op.get_gather();
		const Val &a = V(e.input_operand_id), &ix = V(e.indices_operand_id);
		Shape out = OutShape(e.output_operand_id);
		size_t ax = e.axis, n = a.shape[ax], inner = 1, outer = 1;
		for ( size_t d = ax + 1; d < a.shape.size(); d++ ) inner *= a.shape[d];
		for ( size_t d = 0; d < ax; d++ ) outer *= a.shape[d];
		size_t ni = ix.v.size();
		std::vector<double> r(outer * ni * inner);
		for ( size_t o = 0; o < outer; o++ )
		for ( size_t k = 0; k < ni; k++ ) {
			long q = (long)ix.v[k];
			if ( q < 0 ) q += (long)n;
			q = std::clamp(q, 0L, (long)n - 1);
			for ( size_t in = 0; in < inner; in++ ) r[( o * ni + k ) * inner + in] = a.v[( o * n + q ) * inner + in];
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kGatherElements: {
		const auto &e = *op.get_gather_elements();
		const Val &a = V(e.input_operand_id), &ix = V(e.indices_operand_id);
		auto as = strides(a.shape);
		std::vector<double> r(ix.v.size());
		for ( size_t i = 0; i < r.size(); i++ ) {
			unflat(i, ix.shape, idx);
			long q = (long)ix.v[i];
			if ( q < 0 ) q += a.shape[e.axis];
			idx[e.axis] = (size_t)std::clamp(q, 0L, (long)a.shape[e.axis] - 1);
			size_t f = 0;
			for ( size_t d = 0; d < idx.size(); d++ ) f += idx[d] * as[d];
			r[i] = a.v[f];
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kGatherNd: {
		const auto &e = *op.get_gather_nd();
		const Val &a = V(e.input_operand_id), &ix = V(e.indices_operand_id);
		size_t k = ix.shape.back(), nq = ix.v.size() / ( k ? k : 1 ), inner = 1;
		for ( size_t d = k; d < a.shape.size(); d++ ) inner *= a.shape[d];
		auto as = strides(a.shape);
		std::vector<double> r(nq * inner);
		for ( size_t q = 0; q < nq; q++ ) {
			size_t f = 0;
			for ( size_t d = 0; d < k; d++ ) {
				long v = (long)ix.v[q * k + d];
				if ( v < 0 ) v += a.shape[d];
				f += (size_t)std::clamp(v, 0L, (long)a.shape[d] - 1) * as[d];
			}
			for ( size_t in = 0; in < inner; in++ ) r[q * inner + in] = a.v[f + in];
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kScatterElements: {
		const auto &e = *op.get_scatter_elements();
		const Val &a = V(e.input_operand_id), &ix = V(e.indices_operand_id), &up = V(e.updates_operand_id);
		auto as = strides(a.shape);
		std::vector<double> r(a.v);
		for ( size_t i = 0; i < ix.v.size(); i++ ) {
			unflat(i, ix.shape, idx);
			long q = (long)ix.v[i];
			if ( q < 0 ) q += a.shape[e.axis];
			idx[e.axis] = (size_t)std::clamp(q, 0L, (long)a.shape[e.axis] - 1);
			size_t f = 0;
			for ( size_t d = 0; d < idx.size(); d++ ) f += idx[d] * as[d];
			r[f] = up.v[i];
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kScatterNd: {
		const auto &e = *op.get_scatter_nd();
		const Val &a = V(e.input_operand_id), &ix = V(e.indices_operand_id), &up = V(e.updates_operand_id);
		size_t k = ix.shape.back(), nq = ix.v.size() / ( k ? k : 1 ), inner = 1;
		for ( size_t d = k; d < a.shape.size(); d++ ) inner *= a.shape[d];
		auto as = strides(a.shape);
		std::vector<double> r(a.v);
		for ( size_t q = 0; q < nq; q++ ) {
			size_t f = 0;
			for ( size_t d = 0; d < k; d++ ) {
				long v = (long)ix.v[q * k + d];
				if ( v < 0 ) v += a.shape[d];
				f += (size_t)std::clamp(v, 0L, (long)a.shape[d] - 1) * as[d];
			}
			for ( size_t in = 0; in < inner; in++ ) r[f + in] = up.v[q * inner + in];
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kDequantizeLinear: case Tag::kQuantizeLinear: {
		bool deq = op.which() == Tag::kDequantizeLinear;
		OperandId in = deq ? op.get_dequantize_linear()->input_operand_id : op.get_quantize_linear()->input_operand_id;
		OperandId sc = deq ? op.get_dequantize_linear()->scale_operand_id : op.get_quantize_linear()->scale_operand_id;
		OperandId zp = deq ? op.get_dequantize_linear()->zero_point_operand_id : op.get_quantize_linear()->zero_point_operand_id;
		OperandId out = deq ? op.get_dequantize_linear()->output_operand_id : op.get_quantize_linear()->output_operand_id;
		const Val &a = V(in), &s = V(sc), &z = V(zp);
		auto ss = strides(s.shape), zs = strides(z.shape);
		/* the scale and the zero point, block-wise: each of their elements covers a block of the input */
		auto at = [&]( const Val &p, const std::vector<size_t> &pst ) {
			size_t f = 0, off = a.shape.size() - p.shape.size();
			for ( size_t d = 0; d < p.shape.size(); d++ ) {
				size_t blk = a.shape[d + off] / ( p.shape[d] ? p.shape[d] : 1 );
				f += ( idx[d + off] / ( blk ? blk : 1 ) ) * pst[d];
			}
			return f;
		};
		T ot = OutType(out);
		std::vector<double> r(a.v.size());
		for ( size_t i = 0; i < r.size(); i++ ) {
			unflat(i, a.shape, idx);
			double sv = s.v[at(s, ss)], zv = z.v[at(z, zs)];
			r[i] = deq ? ( a.v[i] - zv ) * sv : fit(round_even(a.v[i] / sv) + zv, ot);
		}
		Put(out, std::move(r));
		return true;
	}
	case Tag::kResample2d: {
		const auto &e = *op.get_resample2d();
		const Val &a = V(e.input_operand_id);
		Shape out = OutShape(e.output_operand_id);
		size_t ha = e.axes[0], wa = e.axes[1];
		double sh = (double)out[ha] / a.shape[ha], sw = (double)out[wa] / a.shape[wa];
		if ( e.scales ) { sh = (*e.scales)[0]; sw = (*e.scales)[1]; }
		auto as = strides(a.shape);
		std::vector<double> r(count(out));
		long IH = a.shape[ha], IW = a.shape[wa];
		for ( size_t i = 0; i < r.size(); i++ ) {
			unflat(i, out, idx);
			double y = ( idx[ha] + 0.5 ) / sh - 0.5, x = ( idx[wa] + 0.5 ) / sw - 0.5;
			auto at = [&]( long yy, long xx ) {
				yy = std::clamp(yy, 0L, IH - 1);
				xx = std::clamp(xx, 0L, IW - 1);
				jdx = idx;
				jdx[ha] = (size_t)yy;
				jdx[wa] = (size_t)xx;
				size_t f = 0;
				for ( size_t d = 0; d < jdx.size(); d++ ) f += jdx[d] * as[d];
				return a.v[f];
			};
			if ( e.mode == mojom::Resample2d::InterpolationMode::kNearestNeighbor ) {
				r[i] = at((long)floor(( idx[ha] + 0.5 ) / sh), (long)floor(( idx[wa] + 0.5 ) / sw));
			} else {
				double y0 = floor(y), x0 = floor(x), fy = y - y0, fx = x - x0;
				r[i] = at((long)y0, (long)x0) * ( 1 - fy ) * ( 1 - fx ) + at((long)y0, (long)x0 + 1) * ( 1 - fy ) * fx
				       + at((long)y0 + 1, (long)x0) * fy * ( 1 - fx ) + at((long)y0 + 1, (long)x0 + 1) * fy * fx;
			}
		}
		Put(e.output_operand_id, std::move(r));
		return true;
	}
	case Tag::kGruCell: {
		const auto &e = *op.get_gru_cell();
		const Val &x = V(e.input_operand_id), &w = V(e.weight_operand_id), &rw = V(e.recurrent_weight_operand_id);
		std::vector<double> h = V(e.hidden_state_operand_id).v;
		const double *B = e.bias_operand_id ? V(*e.bias_operand_id).v.data() : nullptr;
		const double *RB = e.recurrent_bias_operand_id ? V(*e.recurrent_bias_operand_id).v.data() : nullptr;
		std::vector<mojom::RecurrentNetworkActivation> acts(e.activations.begin(), e.activations.end());
		GruCell(x.v, x.shape[0], x.shape[1], w.v.data(), rw.v.data(), B, RB, h, e.hidden_size, e.reset_after, e.layout, acts);
		Put(e.output_operand_id, std::move(h));
		return true;
	}
	case Tag::kLstmCell: {
		const auto &e = *op.get_lstm_cell();
		const Val &x = V(e.input_operand_id), &w = V(e.weight_operand_id), &rw = V(e.recurrent_weight_operand_id);
		std::vector<double> h = V(e.hidden_state_operand_id).v, c = V(e.cell_state_operand_id).v;
		const double *B = e.bias_operand_id ? V(*e.bias_operand_id).v.data() : nullptr;
		const double *RB = e.recurrent_bias_operand_id ? V(*e.recurrent_bias_operand_id).v.data() : nullptr;
		const double *P = e.peephole_weight_operand_id ? V(*e.peephole_weight_operand_id).v.data() : nullptr;
		std::vector<mojom::RecurrentNetworkActivation> acts(e.activations.begin(), e.activations.end());
		LstmCell(x.v, x.shape[0], x.shape[1], w.v.data(), rw.v.data(), B, RB, P, h, c, e.hidden_size, e.layout, acts);
		Put(e.output_operand_ids[0], std::move(h));
		Put(e.output_operand_ids[1], std::move(c));
		return true;
	}
	case Tag::kGru: case Tag::kLstm: {
		bool gru = op.which() == Tag::kGru;
		OperandId in = gru ? op.get_gru()->input_operand_id : op.get_lstm()->input_operand_id;
		const Val &x = V(in);
		size_t steps = x.shape[0], batch = x.shape[1], isz = x.shape[2];
		size_t hs = gru ? op.get_gru()->hidden_size : op.get_lstm()->hidden_size;
		auto dir = gru ? op.get_gru()->direction : op.get_lstm()->direction;
		size_t nd = dir == mojom::RecurrentNetworkDirection::kBoth ? 2 : 1;
		const Val &w = V(gru ? op.get_gru()->weight_operand_id : op.get_lstm()->weight_operand_id);
		const Val &rw = V(gru ? op.get_gru()->recurrent_weight_operand_id : op.get_lstm()->recurrent_weight_operand_id);
		auto opt = [&]( const std::optional<OperandId> &id ) { return id ? &V(*id) : (const Val *)nullptr; };
		const Val *B = opt(gru ? op.get_gru()->bias_operand_id : op.get_lstm()->bias_operand_id);
		const Val *RB = opt(gru ? op.get_gru()->recurrent_bias_operand_id : op.get_lstm()->recurrent_bias_operand_id);
		const Val *H0 = opt(gru ? op.get_gru()->initial_hidden_state_operand_id : op.get_lstm()->initial_hidden_state_operand_id);
		const Val *C0 = gru ? nullptr : opt(op.get_lstm()->initial_cell_state_operand_id);
		const Val *P = gru ? nullptr : opt(op.get_lstm()->peephole_weight_operand_id);
		bool seq = gru ? op.get_gru()->return_sequence : op.get_lstm()->return_sequence;
		size_t gates = gru ? 3 : 4;
		std::vector<mojom::RecurrentNetworkActivation> acts = gru
			? std::vector<mojom::RecurrentNetworkActivation>(op.get_gru()->activations.begin(), op.get_gru()->activations.end())
			: std::vector<mojom::RecurrentNetworkActivation>(op.get_lstm()->activations.begin(), op.get_lstm()->activations.end());
		std::vector<double> hfin(nd * batch * hs), cfin(nd * batch * hs), sequence(seq ? steps * nd * batch * hs : 0);
		for ( size_t d = 0; d < nd; d++ ) {
			bool back = dir == mojom::RecurrentNetworkDirection::kBackward || d == 1;
			std::vector<double> h(batch * hs, 0.0), c(batch * hs, 0.0);
			if ( H0 ) std::copy(H0->v.begin() + d * batch * hs, H0->v.begin() + ( d + 1 ) * batch * hs, h.begin());
			if ( C0 ) std::copy(C0->v.begin() + d * batch * hs, C0->v.begin() + ( d + 1 ) * batch * hs, c.begin());
			const double *Wd = &w.v[d * gates * hs * isz], *Rd = &rw.v[d * gates * hs * hs];
			const double *Bd = B ? &B->v[d * gates * hs] : nullptr, *RBd = RB ? &RB->v[d * gates * hs] : nullptr;
			const double *Pd = P ? &P->v[d * 3 * hs] : nullptr;
			for ( size_t t = 0; t < steps; t++ ) {
				size_t s = back ? steps - 1 - t : t;
				std::vector<double> xs(x.v.begin() + s * batch * isz, x.v.begin() + ( s + 1 ) * batch * isz);
				if ( gru ) GruCell(xs, batch, isz, Wd, Rd, Bd, RBd, h, hs, op.get_gru()->reset_after, op.get_gru()->layout, acts);
				else LstmCell(xs, batch, isz, Wd, Rd, Bd, RBd, Pd, h, c, hs, op.get_lstm()->layout, acts);
				if ( seq ) std::copy(h.begin(), h.end(), sequence.begin() + ( ( s * nd + d ) * batch ) * hs);
			}
			std::copy(h.begin(), h.end(), hfin.begin() + d * batch * hs);
			std::copy(c.begin(), c.end(), cfin.begin() + d * batch * hs);
		}
		const auto &outs = gru ? op.get_gru()->output_operand_ids : op.get_lstm()->output_operand_ids;
		size_t k = 0;
		Put(outs[k++], std::move(hfin));
		if ( !gru ) Put(outs[k++], std::move(cfin));
		if ( seq && k < outs.size() ) Put(outs[k], std::move(sequence));
		return true;
	}
	}
	return false;
}

/* ---------------------------------------------------------------- tensors */

class Tensor final : public WebNNTensorImpl {
public:
	Tensor( mojo::PendingAssociatedReceiver<mojom::WebNNTensor> receiver, WebNNContextImpl &context,
		mojom::TensorInfoPtr info )
		: WebNNTensorImpl(std::move(receiver), context, std::move(info))
	{
		bytes_.assign(PackedByteLength(), 0);
	}

	std::vector<uint8_t> &bytes() { return bytes_; }

private:
	~Tensor() override = default;

	void ReadTensorImpl( mojom::WebNNTensor::ReadTensorCallback callback ) override
	{
		std::move(callback).Run(mojom::ReadTensorResult::NewBuffer(
			context_->WriteDataToDataPipeOrBigBuffer(base::span<const uint8_t>(bytes_))));
	}

	void WriteTensorImpl( mojo_base::BigBuffer src ) override
	{
		context_->ReadDataFromBigBufferOrDataPipe(std::move(src), base::span<uint8_t>(bytes_));
	}

	bool ImportTensorImpl( ScopedAccessPtr ) override { return false; }
	void ExportTensorImpl( ScopedAccessPtr ) override {}

	std::vector<uint8_t>	bytes_;
};

/* ---------------------------------------------------------------- graphs */

class Graph final : public WebNNGraphImpl {
public:
	Graph( WebNNContextImpl &context, ComputeResourceInfo info, mojom::GraphInfoPtr g,
	       base::flat_map<OperandId, std::unique_ptr<WebNNConstantOperand>> constants )
		: WebNNGraphImpl(context, std::move(info), {mojom::Device::kCpu}), g_(std::move(g))
	{
		consts_.resize(g_->operands.size());
		for ( auto &[id, c] : constants ) {
			Val &v = consts_[id.value()];
			v.type = c->descriptor().data_type();
			v.shape.assign(c->descriptor().shape().begin(), c->descriptor().shape().end());
			v.v = load(c->ByteSpan().data(), count(v.shape), v.type);
			is_const_.insert(id.value());
		}
		for ( size_t i = 0; i < g_->operands.size(); i++ ) {
			const auto &o = g_->operands[i];
			if ( o->name ) {
				if ( o->kind == mojom::Operand::Kind::kInput ) inputs_[*o->name] = i;
				if ( o->kind == mojom::Operand::Kind::kOutput ) outputs_[*o->name] = i;
			}
		}
	}

private:
	~Graph() override = default;

	void DispatchImpl( base::flat_map<std::string, scoped_refptr<WebNNTensorImpl>> named_inputs,
			   base::flat_map<std::string, scoped_refptr<WebNNTensorImpl>> named_outputs ) override
	{
		std::vector<Val> vals(g_->operands.size());
		for ( uint32_t i : is_const_ ) vals[i] = consts_[i];
		for ( auto &[name, t] : named_inputs ) {
			auto it = inputs_.find(name);
			if ( it == inputs_.end() ) continue;
			Val &v = vals[it->second];
			const auto &d = g_->operands[it->second]->descriptor;
			v.type = d.data_type();
			v.shape.assign(d.shape().begin(), d.shape().end());
			v.v = load(static_cast<Tensor *>(t.get())->bytes().data(), count(v.shape), v.type);
		}
		std::string err;
		Interpreter run(*g_, vals);
		if ( !run.Run(&err) ) {
			printf("browser: webnn: %s\n", err.c_str());
			return;
		}
		for ( auto &[name, t] : named_outputs ) {
			auto it = outputs_.find(name);
			if ( it == outputs_.end() ) continue;
			const Val &v = vals[it->second];
			auto &b = static_cast<Tensor *>(t.get())->bytes();
			const auto &d = g_->operands[it->second]->descriptor;
			if ( v.v.size() != count(Shape(d.shape().begin(), d.shape().end())) ) continue;
			std::vector<uint8_t> tmp(bytes_of(v.v.size(), d.data_type()));
			store(v.v, tmp.data(), d.data_type());
			memcpy(b.data(), tmp.data(), std::min(b.size(), tmp.size()));
		}
	}

	mojom::GraphInfoPtr			g_;
	std::vector<Val>			consts_;
	base::flat_set<uint32_t>		is_const_;
	std::map<std::string, uint32_t>		inputs_, outputs_;
};

/* ---------------------------------------------------------------- contexts */

ContextProperties Properties()
{
	SupportedTensors t{DataTypeConstraint::kAllDataTypesAtLeast8bits, SupportedRanks{0, 8}};
	SupportedDataTypes logical = DataTypeConstraint::kUint8;
#define T4 t, t, t, t
#define T20 T4, T4, T4, T4, T4
	/* the limits' fields in their order: 35 tensors, the logical operators' output, 82 tensors */
	DataTypeLimits limits(T20, T4, T4, T4, t, t, t, logical, T20, T20, T20, T20, t, t);
#undef T20
#undef T4
	return ContextProperties(InputOperandLayout::kNchw, Resample2DAxes::kAny, BatchNormalizationAxis::kAny,
				 /*tensor_byte_length_limit=*/std::numeric_limits<int32_t>::max(), std::move(limits));
}

class Context final : public WebNNContextImpl {
public:
	Context( mojo::PendingReceiver<mojom::WebNNContext> receiver, base::WeakPtr<WebNNContextProviderInRenderer> provider,
		 mojom::CreateContextOptionsPtr options, scoped_refptr<base::SingleThreadTaskRunner> owning,
		 scoped_refptr<base::SingleThreadTaskRunner> main )
		: WebNNContextImpl(std::move(receiver), std::move(provider), ContextBackendUma::kNotSupported, Properties(),
				   std::move(options), std::move(owning), std::move(main)) {}

	base::WeakPtr<WebNNContextImpl> AsWeakPtr() override { return weak_.GetWeakPtr(); }

private:
	~Context() override = default;

	void CreateGraphImpl( mojom::GraphInfoPtr graph_info, WebNNGraphImpl::ComputeResourceInfo info,
			      base::flat_map<OperandId, std::unique_ptr<WebNNConstantOperand>> constants,
			      CreateGraphImplCallback callback ) override
	{
		std::move(callback).Run(base::MakeRefCounted<Graph>(*this, std::move(info), std::move(graph_info),
								    std::move(constants)));
	}

	base::expected<scoped_refptr<WebNNTensorImpl>, mojom::ErrorPtr> CreateTensorImpl(
		mojo::PendingAssociatedReceiver<mojom::WebNNTensor> receiver, mojom::TensorInfoPtr info ) override
	{
		return base::MakeRefCounted<Tensor>(std::move(receiver), *this, std::move(info));
	}

	base::expected<scoped_refptr<WebNNTensorImpl>, mojom::ErrorPtr> CreateTensorFromSharedImageImpl(
		mojo::PendingAssociatedReceiver<mojom::WebNNTensor>, mojom::TensorInfoPtr,
		WebNNTensorImpl::RepresentationPtr ) override
	{
		return base::unexpected(mojom::Error::New(mojom::Error::Code::kNotSupportedError,
							  "Tensors of WebGPU are not supported."));
	}

	std::string_view GetBackendName() const override { return "TessronOS CPU"; }

	std::vector<mojom::WebNNExecutionProviderDetailsPtr> GetExecutionProvidersInfo() const override { return {}; }

	base::WeakPtrFactory<Context>	weak_{this};
};

WebNNContextImpl::WebNNContextImplPtr Make( mojo::PendingReceiver<mojom::WebNNContext> receiver,
					    base::WeakPtr<WebNNContextProviderInRenderer> provider,
					    mojom::CreateContextOptionsPtr options,
					    scoped_refptr<base::SingleThreadTaskRunner> owning,
					    scoped_refptr<base::SingleThreadTaskRunner> main )
{
	auto runner = owning;
	return WebNNContextImpl::WebNNContextImplPtr(
		new Context(std::move(receiver), std::move(provider), std::move(options), std::move(owning), std::move(main)),
		OnTaskRunnerDeleter(std::move(runner)));
}

}  // namespace
}  // namespace tessronos
}  // namespace webnn

/* The binder of the frames' broker, once when the engine is up */
void br_webnn_init()
{
	webnn::g_cpu_context_for_renderer = &webnn::tessronos::Make;
	blink::GetEmptyBrowserInterfaceBroker().SetBinderForTesting(
		webnn::mojom::WebNNContextProvider::Name_, base::BindRepeating([]( mojo::ScopedMessagePipeHandle h ) {
			br_on_service_thread(base::BindOnce([]( mojo::ScopedMessagePipeHandle h ) {
				mojo::MakeSelfOwnedReceiver(
					std::make_unique<webnn::WebNNContextProviderInRenderer>(
						mojo::NullRemote(), base::SingleThreadTaskRunner::GetCurrentDefault()),
					mojo::PendingReceiver<webnn::mojom::WebNNContextProvider>(std::move(h)));
			}, std::move(h)));
		}));
}

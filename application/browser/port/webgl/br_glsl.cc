/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_glsl.cc
 *	ブラウザ: the GLSL ES pipeline on the CPU (br_glsl.h). Every value is
 *	held as floats; ints and bools are floats with integral values.
 */

#include <stdio.h>
#include "br_glsl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <memory>


namespace brgl {
namespace {

// ---- lexer -----------------------------------------------------------------

enum TokKind { TK_END, TK_IDENT, TK_NUM, TK_PUNCT };

struct Tok {
  TokKind kind = TK_END;
  std::string text;
  float num = 0;
};

struct Lexer {
  const std::string& s;
  size_t p = 0;
  explicit Lexer(const std::string& src) : s(src) {}

  void SkipWs() {
    for (;;) {
      while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' ||
                              s[p] == '\n'))
        ++p;
      if (p + 1 < s.size() && s[p] == '/' && s[p + 1] == '/') {
        while (p < s.size() && s[p] != '\n') ++p;
        continue;
      }
      if (p + 1 < s.size() && s[p] == '/' && s[p + 1] == '*') {
        p += 2;
        while (p + 1 < s.size() && !(s[p] == '*' && s[p + 1] == '/')) ++p;
        p = p + 2 <= s.size() ? p + 2 : s.size();
        continue;
      }
      if (p < s.size() && s[p] == '#') {  // preprocessor line: ignore
        while (p < s.size() && s[p] != '\n') ++p;
        continue;
      }
      break;
    }
  }

  Tok Next() {
    SkipWs();
    Tok t;
    if (p >= s.size())
      return t;
    char c = s[p];
    if (isalpha((unsigned char)c) || c == '_') {
      size_t b = p;
      while (p < s.size() &&
             (isalnum((unsigned char)s[p]) || s[p] == '_'))
        ++p;
      t.kind = TK_IDENT;
      t.text = s.substr(b, p - b);
      return t;
    }
    if (isdigit((unsigned char)c) ||
        (c == '.' && p + 1 < s.size() && isdigit((unsigned char)s[p + 1]))) {
      size_t b = p;
      while (p < s.size() && (isdigit((unsigned char)s[p]) || s[p] == '.'))
        ++p;
      if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
        ++p;
        if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
        while (p < s.size() && isdigit((unsigned char)s[p])) ++p;
      }
      if (p < s.size() && (s[p] == 'f' || s[p] == 'F' || s[p] == 'u' ||
                           s[p] == 'U'))
        ++p;
      t.kind = TK_NUM;
      t.text = s.substr(b, p - b);
      t.num = (float)atof(t.text.c_str());
      return t;
    }
    // multi-char punctuation, longest first
    static const char* two[] = {"==", "!=", "<=", ">=", "&&", "||", "^^",
                                "+=", "-=", "*=", "/=", "++", "--"};
    for (const char* d : two) {
      if (s.compare(p, 2, d) == 0) {
        t.kind = TK_PUNCT;
        t.text = d;
        p += 2;
        return t;
      }
    }
    t.kind = TK_PUNCT;
    t.text = std::string(1, c);
    ++p;
    return t;
  }
};

// ---- program structure -----------------------------------------------------

enum RefKind { RK_LOCAL, RK_GLOBAL, RK_UNIFORM, RK_ATTRIB, RK_VARYING,
               RK_BUILTIN };

// builtin variable ids
enum { BI_POSITION, BI_POINTSIZE, BI_FRAGCOLOR, BI_FRAGCOORD, BI_FRONTFACING,
       BI_COUNT };

struct Expr;
struct Stmt;
using ExprP = std::unique_ptr<Expr>;
using StmtP = std::unique_ptr<Stmt>;

struct FuncDef {
  ValType ret = T_VOID;
  std::vector<int> param_slots;
  std::vector<ValType> param_types;
  std::vector<StmtP> body;
  int frame_size = 0;
};

}  // namespace

class Program {
 public:
  std::vector<VarInfo> attribs;
  std::vector<VarInfo> uniforms;
  std::vector<VarInfo> varyings;
  std::vector<VarInfo> globals;                 // both stages share the space
  std::vector<std::pair<int, Expr*>> vs_ginit;  // global slot, initializer
  std::vector<std::pair<int, Expr*>> fs_ginit;
  std::map<std::string, FuncDef> vs_funcs;
  std::map<std::string, FuncDef> fs_funcs;
  std::vector<ExprP> owned_exprs;  // keeps initializer expressions alive
  ~Program();
};

namespace {

// ---- AST -------------------------------------------------------------------

struct EvalCtx;

struct Expr {
  virtual ~Expr() {}
  virtual Val Eval(EvalCtx& c) = 0;
  // lvalue write target: base pointer + component map
  virtual float* LBase(EvalCtx& c, int* comps, int* map) { return nullptr; }
  ValType type = T_FLOAT;  // static type where known
};

enum Flow { F_NORMAL, F_RETURN, F_BREAK, F_CONTINUE, F_DISCARD };

struct Stmt {
  virtual ~Stmt() {}
  virtual Flow Exec(EvalCtx& c) = 0;
};

struct EvalCtx {
  Program* prog;
  bool fragment_stage = false;
  std::vector<Val>* globals;
  const Val* uniforms;
  int uniform_count;
  const Val* attribs;      // VS in
  Val* varyings;           // VS out / FS in
  Val builtins[BI_COUNT];
  const DrawParams* dp;
  std::vector<std::vector<Val>> frames;
  Val ret;
  int depth = 0;

  Val* Slot(RefKind k, int slot) {
    switch (k) {
      case RK_LOCAL:
        return &frames.back()[slot];
      case RK_GLOBAL:
        return &(*globals)[slot];
      case RK_VARYING:
        return &varyings[slot];
      case RK_BUILTIN:
        return &builtins[slot];
      default:
        return nullptr;
    }
  }
};

Val MakeF(float x) {
  Val v;
  v.type = T_FLOAT;
  v.f[0] = x;
  return v;
}

ValType VecOf(int n) {
  return n <= 1 ? T_FLOAT : (ValType)(T_VEC2 + (n - 2));
}

// ---- expression nodes ------------------------------------------------------

struct NumExpr : Expr {
  float v;
  explicit NumExpr(float x) : v(x) { type = T_FLOAT; }
  Val Eval(EvalCtx&) override { return MakeF(v); }
};

struct VarExpr : Expr {
  RefKind kind;
  int slot;
  VarExpr(RefKind k, int s, ValType t) : kind(k), slot(s) { type = t; }
  Val Eval(EvalCtx& c) override {
    if (kind == RK_UNIFORM) {
      Val v;
      if (slot < c.uniform_count)
        v = c.uniforms[slot];
      v.type = type;
      return v;
    }
    if (kind == RK_ATTRIB)
      return c.attribs[slot];
    Val* p = c.Slot(kind, slot);
    return p ? *p : Val();
  }
  float* LBase(EvalCtx& c, int* comps, int* map) override {
    Val* p = c.Slot(kind, slot);
    if (p == nullptr)
      return nullptr;
    p->type = type;
    *comps = Components(type);
    for (int i = 0; i < *comps && i < 4; ++i)
      map[i] = i;
    return p->f;
  }
};

struct SwizzleExpr : Expr {
  ExprP base;
  int map[4];
  int n;
  SwizzleExpr(ExprP b, const int* m, int cnt) : base(std::move(b)), n(cnt) {
    memcpy(map, m, sizeof(map));
    type = VecOf(n);
  }
  Val Eval(EvalCtx& c) override {
    Val b = base->Eval(c);
    Val r;
    r.type = type;
    for (int i = 0; i < n; ++i)
      r.f[i] = b.f[map[i]];
    return r;
  }
  float* LBase(EvalCtx& c, int* comps, int* outmap) override {
    int bc, bmap[4];
    float* p = base->LBase(c, &bc, bmap);
    if (p == nullptr)
      return nullptr;
    *comps = n;
    for (int i = 0; i < n; ++i)
      outmap[i] = bmap[map[i]];
    return p;
  }
};

struct IndexExpr : Expr {
  ExprP base, idx;
  IndexExpr(ExprP b, ExprP i) : base(std::move(b)), idx(std::move(i)) {
    // mat[i] -> column vector; vec[i] -> float
    ValType bt = base->type;
    if (bt == T_MAT2) type = T_VEC2;
    else if (bt == T_MAT3) type = T_VEC3;
    else if (bt == T_MAT4) type = T_VEC4;
    else type = T_FLOAT;
  }
  Val Eval(EvalCtx& c) override {
    Val b = base->Eval(c);
    int i = (int)(idx->Eval(c).f[0] + 0.5f);
    Val r;
    r.type = type;
    int cols = Components(type);
    if (cols == 1) {
      if (i >= 0 && i < 16)
        r.f[0] = b.f[i];
    } else {
      for (int k = 0; k < cols; ++k)
        r.f[k] = b.f[i * cols + k];
    }
    return r;
  }
  float* LBase(EvalCtx& c, int* comps, int* map) override {
    int bc, bmap[4];
    // only vec[i] = x and direct mat[i] on plain vars supported
    float* p = base->LBase(c, &bc, bmap);
    if (p == nullptr)
      return nullptr;
    int i = (int)(idx->Eval(c).f[0] + 0.5f);
    int cols = Components(type);
    *comps = cols;
    if (cols == 1) {
      map[0] = bmap[0] + i;  // vec component
      // matrices as lvalue-of-column handled below
      if (base->type >= T_MAT2 && base->type <= T_MAT4)
        map[0] = i;
    } else {
      for (int k = 0; k < cols; ++k)
        map[k] = i * cols + k;
    }
    return p;
  }
};

struct AssignExpr : Expr {
  ExprP lhs, rhs;
  char op;  // '=', '+', '-', '*', '/'
  AssignExpr(ExprP l, ExprP r, char o)
      : lhs(std::move(l)), rhs(std::move(r)), op(o) {
    type = lhs->type;
  }
  Val Eval(EvalCtx& c) override;
};

struct UnaryExpr : Expr {
  ExprP e;
  char op;  // '-', '!'
  UnaryExpr(ExprP x, char o) : e(std::move(x)), op(o) { type = e->type; }
  Val Eval(EvalCtx& c) override {
    Val v = e->Eval(c);
    int n = Components(v.type);
    if (op == '-')
      for (int i = 0; i < n; ++i) v.f[i] = -v.f[i];
    else if (op == '!')
      v.f[0] = v.f[0] != 0 ? 0.f : 1.f;
    return v;
  }
};

struct IncDecExpr : Expr {
  ExprP e;
  int delta;
  bool post;
  IncDecExpr(ExprP x, int d, bool po) : e(std::move(x)), delta(d), post(po) {
    type = e->type;
  }
  Val Eval(EvalCtx& c) override {
    int comps, map[4];
    float* p = e->LBase(c, &comps, map);
    Val old;
    old.type = type;
    if (p != nullptr) {
      for (int i = 0; i < comps; ++i) {
        old.f[i] = p[map[i]];
        p[map[i]] += (float)delta;
      }
      if (!post)
        for (int i = 0; i < comps; ++i)
          old.f[i] = p[map[i]];
    }
    return old;
  }
};

Val NumBinop(const Val& a, const Val& b, char op) {
  // matrix algebra first
  Val r;
  if (op == '*') {
    auto matn = [](ValType t) {
      return t == T_MAT2 ? 2 : t == T_MAT3 ? 3 : t == T_MAT4 ? 4 : 0;
    };
    int ma = matn(a.type), mb = matn(b.type);
    if (ma && mb && ma == mb) {
      r.type = a.type;
      for (int col = 0; col < ma; ++col)
        for (int row = 0; row < ma; ++row) {
          float s = 0;
          for (int k = 0; k < ma; ++k)
            s += a.f[k * ma + row] * b.f[col * ma + k];
          r.f[col * ma + row] = s;
        }
      return r;
    }
    if (ma && Components(b.type) == ma && !mb) {  // mat * vec
      r.type = b.type;
      for (int row = 0; row < ma; ++row) {
        float s = 0;
        for (int k = 0; k < ma; ++k)
          s += a.f[k * ma + row] * b.f[k];
        r.f[row] = s;
      }
      return r;
    }
    if (mb && Components(a.type) == mb && !ma) {  // vec * mat
      r.type = a.type;
      for (int col = 0; col < mb; ++col) {
        float s = 0;
        for (int k = 0; k < mb; ++k)
          s += a.f[k] * b.f[col * mb + k];
        r.f[col] = s;
      }
      return r;
    }
  }
  int na = Components(a.type), nb = Components(b.type);
  int n = na > nb ? na : nb;
  r.type = na >= nb ? a.type : b.type;
  for (int i = 0; i < n; ++i) {
    float x = a.f[na == 1 ? 0 : i];
    float y = b.f[nb == 1 ? 0 : i];
    switch (op) {
      case '+': r.f[i] = x + y; break;
      case '-': r.f[i] = x - y; break;
      case '*': r.f[i] = x * y; break;
      case '/': r.f[i] = y != 0 ? x / y : 0; break;
    }
  }
  return r;
}

struct BinExpr : Expr {
  ExprP a, b;
  std::string op;
  BinExpr(ExprP x, ExprP y, const std::string& o)
      : a(std::move(x)), b(std::move(y)), op(o) {
    if (o == "+" || o == "-" || o == "*" || o == "/") {
      auto matn = [](ValType t) {
        return t == T_MAT2 || t == T_MAT3 || t == T_MAT4;
      };
      if (o == "*" && matn(a->type) && !matn(b->type) &&
          Components(b->type) > 1)
        type = b->type;
      else
        type = Components(a->type) >= Components(b->type) ? a->type : b->type;
    } else {
      type = T_BOOL;
    }
  }
  Val Eval(EvalCtx& c) override {
    if (op == "&&") {
      Val x = a->Eval(c);
      if (x.f[0] == 0) return MakeF(0);
      return MakeF(b->Eval(c).f[0] != 0 ? 1.f : 0.f);
    }
    if (op == "||") {
      Val x = a->Eval(c);
      if (x.f[0] != 0) return MakeF(1);
      return MakeF(b->Eval(c).f[0] != 0 ? 1.f : 0.f);
    }
    Val x = a->Eval(c);
    Val y = b->Eval(c);
    if (op.size() == 1 && (op[0] == '+' || op[0] == '-' || op[0] == '*' ||
                           op[0] == '/')) {
      Val r = NumBinop(x, y, op[0]);
      r.type = type;
      return r;
    }
    float xf = x.f[0], yf = y.f[0];
    bool res = false;
    if (op == "==") {
      res = true;
      int n = Components(x.type);
      for (int i = 0; i < n; ++i)
        if (x.f[i] != y.f[i]) res = false;
    } else if (op == "!=") {
      res = false;
      int n = Components(x.type);
      for (int i = 0; i < n; ++i)
        if (x.f[i] != y.f[i]) res = true;
    } else if (op == "<") res = xf < yf;
    else if (op == ">") res = xf > yf;
    else if (op == "<=") res = xf <= yf;
    else if (op == ">=") res = xf >= yf;
    else if (op == "^^") res = (xf != 0) != (yf != 0);
    return MakeF(res ? 1.f : 0.f);
  }
};

struct CondExpr : Expr {
  ExprP c_, t_, f_;
  CondExpr(ExprP c, ExprP t, ExprP f)
      : c_(std::move(c)), t_(std::move(t)), f_(std::move(f)) {
    type = t_->type;
  }
  Val Eval(EvalCtx& c) override {
    return c_->Eval(c).f[0] != 0 ? t_->Eval(c) : f_->Eval(c);
  }
};

struct ConstructExpr : Expr {
  std::vector<ExprP> args;
  ConstructExpr(ValType t, std::vector<ExprP> a) : args(std::move(a)) {
    type = t;
  }
  Val Eval(EvalCtx& c) override {
    Val r;
    r.type = type;
    int need = Components(type);
    bool ismat = type >= T_MAT2 && type <= T_MAT4;
    // gather flattened args
    float flat[64];
    int got = 0;
    for (auto& a : args) {
      Val v = a->Eval(c);
      int n = Components(v.type);
      for (int i = 0; i < n && got < 64; ++i)
        flat[got++] = v.f[i];
    }
    if (ismat && got == 1) {  // diagonal
      int dim = type == T_MAT2 ? 2 : type == T_MAT3 ? 3 : 4;
      for (int col = 0; col < dim; ++col)
        for (int row = 0; row < dim; ++row)
          r.f[col * dim + row] = col == row ? flat[0] : 0.f;
      return r;
    }
    if (ismat && args.size() == 1 && got > need) {
      // mat3(mat4) style truncation
      int sdim = (int)sqrtf((float)got);
      int ddim = type == T_MAT2 ? 2 : type == T_MAT3 ? 3 : 4;
      for (int colu = 0; colu < ddim; ++colu)
        for (int row = 0; row < ddim; ++row)
          r.f[colu * ddim + row] = flat[colu * sdim + row];
      return r;
    }
    if (got == 1 && need > 1) {  // splat
      for (int i = 0; i < need; ++i)
        r.f[i] = flat[0];
      return r;
    }
    for (int i = 0; i < need; ++i)
      r.f[i] = i < got ? flat[i] : (i == 15 || i == need - 1 ? 1.f : 0.f);
    if (type == T_INT)
      r.f[0] = (float)(int)r.f[0];
    if (type == T_BOOL)
      r.f[0] = r.f[0] != 0 ? 1.f : 0.f;
    return r;
  }
};

Val SampleTexture(EvalCtx& c, int unit, float u, float v, float bias);

struct CallExpr : Expr {
  std::string name;
  std::vector<ExprP> args;
  const FuncDef* fn = nullptr;  // user function (per stage, fixed at parse)
  CallExpr(const std::string& n, std::vector<ExprP> a, const FuncDef* f,
           ValType t)
      : name(n), args(std::move(a)), fn(f) {
    type = t;
  }
  Val Eval(EvalCtx& c) override;
};

// ---- statements ------------------------------------------------------------

struct ExprStmt : Stmt {
  ExprP e;
  explicit ExprStmt(ExprP x) : e(std::move(x)) {}
  Flow Exec(EvalCtx& c) override {
    e->Eval(c);
    return F_NORMAL;
  }
};

struct DeclStmt : Stmt {
  int slot;
  ValType t;
  ExprP init;
  DeclStmt(int s, ValType ty, ExprP i) : slot(s), t(ty), init(std::move(i)) {}
  Flow Exec(EvalCtx& c) override {
    Val v;
    v.type = t;
    if (init) {
      v = init->Eval(c);
      v.type = t;
    }
    c.frames.back()[slot] = v;
    return F_NORMAL;
  }
};

struct BlockStmt : Stmt {
  std::vector<StmtP> body;
  Flow Exec(EvalCtx& c) override {
    for (auto& s : body) {
      Flow f = s->Exec(c);
      if (f != F_NORMAL)
        return f;
    }
    return F_NORMAL;
  }
};

struct IfStmt : Stmt {
  ExprP cond;
  StmtP then_s, else_s;
  Flow Exec(EvalCtx& c) override {
    if (cond->Eval(c).f[0] != 0)
      return then_s->Exec(c);
    if (else_s)
      return else_s->Exec(c);
    return F_NORMAL;
  }
};

struct ForStmt : Stmt {
  StmtP init;
  ExprP cond, step;
  StmtP body;
  Flow Exec(EvalCtx& c) override {
    if (init)
      init->Exec(c);
    for (int guard = 0; guard < 65536; ++guard) {
      if (cond && cond->Eval(c).f[0] == 0)
        break;
      Flow f = body->Exec(c);
      if (f == F_RETURN || f == F_DISCARD)
        return f;
      if (f == F_BREAK)
        break;
      if (step)
        step->Eval(c);
      if (!cond && f == F_NORMAL && guard == 65535)
        break;
    }
    return F_NORMAL;
  }
};

struct WhileStmt : Stmt {
  ExprP cond;
  StmtP body;
  Flow Exec(EvalCtx& c) override {
    for (int guard = 0; guard < 65536; ++guard) {
      if (cond->Eval(c).f[0] == 0)
        break;
      Flow f = body->Exec(c);
      if (f == F_RETURN || f == F_DISCARD)
        return f;
      if (f == F_BREAK)
        break;
    }
    return F_NORMAL;
  }
};

struct RetStmt : Stmt {
  ExprP e;
  Flow Exec(EvalCtx& c) override {
    if (e)
      c.ret = e->Eval(c);
    return F_RETURN;
  }
};

struct SimpleStmt : Stmt {
  Flow flow;
  explicit SimpleStmt(Flow f) : flow(f) {}
  Flow Exec(EvalCtx&) override { return flow; }
};

Val AssignExpr::Eval(EvalCtx& c) {
  Val r = rhs->Eval(c);
  if (op != '=') {
    Val cur = lhs->Eval(c);
    r = NumBinop(cur, r, op);
  }
  int comps, map[4];
  float* p = lhs->LBase(c, &comps, map);
  if (p != nullptr) {
    int rn = Components(r.type);
    for (int i = 0; i < comps; ++i)
      p[map[i]] = r.f[rn == 1 ? 0 : i];
  }
  return r;
}

// ---- builtins --------------------------------------------------------------

Val Apply1(const Val& a, float (*fn)(float)) {
  Val r = a;
  int n = Components(a.type);
  for (int i = 0; i < n; ++i)
    r.f[i] = fn(a.f[i]);
  return r;
}

Val Apply2(const Val& a, const Val& b, float (*fn)(float, float)) {
  Val r;
  int na = Components(a.type), nb = Components(b.type);
  r.type = na >= nb ? a.type : b.type;
  int n = Components(r.type);
  for (int i = 0; i < n; ++i)
    r.f[i] = fn(a.f[na == 1 ? 0 : i], b.f[nb == 1 ? 0 : i]);
  return r;
}

float fract_f(float x) { return x - floorf(x); }
float sign_f(float x) { return x > 0 ? 1.f : x < 0 ? -1.f : 0.f; }
float inversesqrt_f(float x) { return x > 0 ? 1.f / sqrtf(x) : 0.f; }
float min_f(float a, float b) { return a < b ? a : b; }
float max_f(float a, float b) { return a > b ? a : b; }
float mod_f(float a, float b) { return b != 0 ? a - b * floorf(a / b) : 0; }
float step_f(float e, float x) { return x < e ? 0.f : 1.f; }
float atan2_f(float y, float x) { return atan2f(y, x); }

Val SampleTexture(EvalCtx& c, int unit, float u, float v, float) {
  Val r;
  r.type = T_VEC4;
  r.f[3] = 1;
  if (unit < 0 || unit >= 8)
    return r;
  const TexBinding& t = c.dp->textures[unit];
  if (t.px == nullptr || t.w <= 0 || t.h <= 0)
    return r;
  auto wrap = [](float x, bool repeat) {
    if (repeat)
      return x - floorf(x);
    return x < 0 ? 0.f : x > 1 ? 1.f : x;
  };
  u = wrap(u, t.repeat_s);
  v = wrap(v, t.repeat_t);
  auto fetch = [&](int x, int y, float* out) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= t.w) x = t.w - 1;
    if (y >= t.h) y = t.h - 1;
    const unsigned char* s = t.px + ((size_t)y * t.w + x) * 4;
    for (int i = 0; i < 4; ++i)
      out[i] = s[i] / 255.f;
  };
  if (!t.linear) {
    fetch((int)(u * t.w), (int)(v * t.h), r.f);
    return r;
  }
  float fx = u * t.w - 0.5f;
  float fy = v * t.h - 0.5f;
  int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
  float ax = fx - x0, ay = fy - y0;
  float c00[4], c10[4], c01[4], c11[4];
  fetch(x0, y0, c00);
  fetch(x0 + 1, y0, c10);
  fetch(x0, y0 + 1, c01);
  fetch(x0 + 1, y0 + 1, c11);
  for (int i = 0; i < 4; ++i) {
    float top = c00[i] + (c10[i] - c00[i]) * ax;
    float bot = c01[i] + (c11[i] - c01[i]) * ax;
    r.f[i] = top + (bot - top) * ay;
  }
  return r;
}

Val CallExpr::Eval(EvalCtx& c) {
  // user-defined function
  if (fn != nullptr) {
    if (c.depth > 32)
      return Val();
    std::vector<Val> argv(args.size());
    for (size_t i = 0; i < args.size(); ++i)
      argv[i] = args[i]->Eval(c);
    c.frames.emplace_back(fn->frame_size);
    for (size_t i = 0; i < fn->param_slots.size() && i < argv.size(); ++i) {
      argv[i].type = fn->param_types[i];
      c.frames.back()[fn->param_slots[i]] = argv[i];
    }
    ++c.depth;
    c.ret = Val();
    c.ret.type = fn->ret;
    for (auto& s : fn->body) {
      Flow f = s->Exec(c);
      if (f == F_RETURN || f == F_DISCARD)
        break;
    }
    --c.depth;
    Val r = c.ret;
    r.type = type;
    c.frames.pop_back();
    return r;
  }
  // builtins
  Val a0 = args.size() > 0 ? args[0]->Eval(c) : Val();
  Val a1 = args.size() > 1 ? args[1]->Eval(c) : Val();
  Val a2 = args.size() > 2 ? args[2]->Eval(c) : Val();
  const std::string& n = name;
  if (n == "texture2D" || n == "texture2DLod" || n == "textureCube" ||
      n == "texture2DProj") {
    float u = a1.f[0], v = a1.f[1];
    if (n == "texture2DProj") {
      int nc = Components(a1.type);
      float w = a1.f[nc - 1];
      if (w != 0) { u /= w; v /= w; }
    }
    return SampleTexture(c, (int)(a0.f[0] + 0.5f), u, v, 0);
  }
  if (n == "sin") return Apply1(a0, sinf);
  if (n == "cos") return Apply1(a0, cosf);
  if (n == "tan") return Apply1(a0, tanf);
  if (n == "asin") return Apply1(a0, asinf);
  if (n == "acos") return Apply1(a0, acosf);
  if (n == "atan")
    return args.size() == 2 ? Apply2(a0, a1, atan2_f) : Apply1(a0, atanf);
  if (n == "radians") return Apply1(a0, [](float x) { return x * 0.017453292f; });
  if (n == "degrees") return Apply1(a0, [](float x) { return x * 57.29578f; });
  if (n == "exp") return Apply1(a0, expf);
  if (n == "log") return Apply1(a0, logf);
  if (n == "exp2") return Apply1(a0, exp2f);
  if (n == "log2") return Apply1(a0, log2f);
  if (n == "sqrt") return Apply1(a0, sqrtf);
  if (n == "inversesqrt") return Apply1(a0, inversesqrt_f);
  if (n == "abs") return Apply1(a0, fabsf);
  if (n == "sign") return Apply1(a0, sign_f);
  if (n == "floor") return Apply1(a0, floorf);
  if (n == "ceil") return Apply1(a0, ceilf);
  if (n == "fract") return Apply1(a0, fract_f);
  if (n == "pow") return Apply2(a0, a1, powf);
  if (n == "mod") return Apply2(a0, a1, mod_f);
  if (n == "min") return Apply2(a0, a1, min_f);
  if (n == "max") return Apply2(a0, a1, max_f);
  if (n == "step") return Apply2(a0, a1, step_f);
  if (n == "clamp") {
    Val lo = Apply2(a0, a1, max_f);
    return Apply2(lo, a2, min_f);
  }
  if (n == "mix") {
    Val r;
    int n0 = Components(a0.type), n2 = Components(a2.type);
    r.type = a0.type;
    int cnt = Components(r.type);
    for (int i = 0; i < cnt; ++i) {
      float t = a2.f[n2 == 1 ? 0 : i];
      r.f[i] = a0.f[i] + (a1.f[n0 == 1 ? 0 : i] - a0.f[i]) * t;
    }
    return r;
  }
  if (n == "smoothstep") {
    Val r;
    r.type = a2.type;
    int cnt = Components(r.type);
    int n0 = Components(a0.type);
    for (int i = 0; i < cnt; ++i) {
      float e0 = a0.f[n0 == 1 ? 0 : i];
      float e1 = a1.f[n0 == 1 ? 0 : i];
      float t = e1 != e0 ? (a2.f[i] - e0) / (e1 - e0) : 0;
      t = t < 0 ? 0 : t > 1 ? 1 : t;
      r.f[i] = t * t * (3 - 2 * t);
    }
    return r;
  }
  if (n == "length" || n == "distance") {
    float s = 0;
    int cnt = Components(a0.type);
    for (int i = 0; i < cnt; ++i) {
      float d = n == "distance" ? a0.f[i] - a1.f[i] : a0.f[i];
      s += d * d;
    }
    return MakeF(sqrtf(s));
  }
  if (n == "dot") {
    float s = 0;
    int cnt = Components(a0.type);
    for (int i = 0; i < cnt; ++i)
      s += a0.f[i] * a1.f[i];
    return MakeF(s);
  }
  if (n == "cross") {
    Val r;
    r.type = T_VEC3;
    r.f[0] = a0.f[1] * a1.f[2] - a0.f[2] * a1.f[1];
    r.f[1] = a0.f[2] * a1.f[0] - a0.f[0] * a1.f[2];
    r.f[2] = a0.f[0] * a1.f[1] - a0.f[1] * a1.f[0];
    return r;
  }
  if (n == "normalize") {
    float s = 0;
    int cnt = Components(a0.type);
    for (int i = 0; i < cnt; ++i)
      s += a0.f[i] * a0.f[i];
    float inv = s > 0 ? 1.f / sqrtf(s) : 0;
    Val r = a0;
    for (int i = 0; i < cnt; ++i)
      r.f[i] *= inv;
    return r;
  }
  if (n == "reflect") {
    // I - 2*dot(N,I)*N
    float d = 0;
    int cnt = Components(a0.type);
    for (int i = 0; i < cnt; ++i)
      d += a1.f[i] * a0.f[i];
    Val r = a0;
    for (int i = 0; i < cnt; ++i)
      r.f[i] = a0.f[i] - 2 * d * a1.f[i];
    return r;
  }
  if (n == "matrixCompMult") {
    Val r = a0;
    int cnt = Components(a0.type);
    for (int i = 0; i < cnt; ++i)
      r.f[i] *= a1.f[i];
    return r;
  }
  return Val();
}

// ---- parser / compiler -----------------------------------------------------

struct RefInfo {
  RefKind kind;
  int slot;
  ValType type;
};

struct Compiler {
  Program* prog;
  bool fragment;
  Lexer lex;
  Tok tok;
  bool failed = false;
  std::string err;
  std::vector<std::map<std::string, RefInfo>> scopes;
  int frame_top = 0;
  int frame_max = 0;
  std::map<std::string, FuncDef>* funcs;
  std::vector<std::pair<int, Expr*>>* ginit;

  Compiler(Program* p, const std::string& src, bool frag)
      : prog(p), fragment(frag), lex(src) {
    funcs = frag ? &p->fs_funcs : &p->vs_funcs;
    ginit = frag ? &p->fs_ginit : &p->vs_ginit;
    tok = lex.Next();
    scopes.emplace_back();
  }

  void Fail(const std::string& m) {
    if (!failed) {
      failed = true;
      err = m + " near '" + tok.text + "'";
    }
  }
  void Advance() { tok = lex.Next(); }
  bool Is(const char* t) { return tok.text == t; }
  bool Accept(const char* t) {
    if (Is(t)) {
      Advance();
      return true;
    }
    return false;
  }
  void Expect(const char* t) {
    if (!Accept(t))
      Fail(std::string("expected ") + t);
  }

  bool IsType(const std::string& s, ValType* out) {
    if (s == "float") *out = T_FLOAT;
    else if (s == "int") *out = T_INT;
    else if (s == "bool") *out = T_BOOL;
    else if (s == "vec2" || s == "ivec2" || s == "bvec2") *out = T_VEC2;
    else if (s == "vec3" || s == "ivec3" || s == "bvec3") *out = T_VEC3;
    else if (s == "vec4" || s == "ivec4" || s == "bvec4") *out = T_VEC4;
    else if (s == "mat2") *out = T_MAT2;
    else if (s == "mat3") *out = T_MAT3;
    else if (s == "mat4") *out = T_MAT4;
    else if (s == "sampler2D") *out = T_SAMPLER2D;
    else if (s == "samplerCube") *out = T_SAMPLERCUBE;
    else if (s == "void") *out = T_VOID;
    else return false;
    return true;
  }

  RefInfo* Lookup(const std::string& n) {
    for (int i = (int)scopes.size() - 1; i >= 0; --i) {
      auto it = scopes[i].find(n);
      if (it != scopes[i].end())
        return &it->second;
    }
    return nullptr;
  }

  int FindVar(std::vector<VarInfo>& tbl, const std::string& n, ValType t,
              bool add) {
    for (size_t i = 0; i < tbl.size(); ++i)
      if (tbl[i].name == n)
        return (int)i;
    if (!add)
      return -1;
    tbl.push_back({n, t});
    return (int)tbl.size() - 1;
  }

  // -------- expressions --------

  ExprP ParsePrimary() {
    if (tok.kind == TK_NUM) {
      float v = tok.num;
      Advance();
      return ExprP(new NumExpr(v));
    }
    if (Accept("(")) {
      ExprP e = ParseExpr();
      Expect(")");
      return e;
    }
    if (Accept("true"))
      return ExprP(new NumExpr(1));
    if (Accept("false"))
      return ExprP(new NumExpr(0));
    if (tok.kind == TK_IDENT) {
      std::string name = tok.text;
      ValType ty;
      Advance();
      if (Is("(")) {
        // constructor or call
        Advance();
        std::vector<ExprP> args;
        if (!Is(")")) {
          do {
            args.push_back(ParseAssign());
          } while (Accept(","));
        }
        Expect(")");
        if (IsType(name, &ty))
          return ExprP(new ConstructExpr(ty, std::move(args)));
        const FuncDef* fd = nullptr;
        ValType rt = T_VEC4;
        auto it = funcs->find(name);
        if (it != funcs->end()) {
          fd = &it->second;
          rt = fd->ret;
        } else {
          // builtin return typing: component-wise keep arg0 type
          if (name == "texture2D" || name == "textureCube" ||
              name == "texture2DProj" || name == "texture2DLod")
            rt = T_VEC4;
          else if (name == "length" || name == "distance" || name == "dot")
            rt = T_FLOAT;
          else if (name == "cross")
            rt = T_VEC3;
          else if (!args.empty())
            rt = name == "mix" || name == "clamp" || name == "smoothstep"
                     ? (name == "smoothstep" ? args.back()->type
                                             : args[0]->type)
                     : args[0]->type;
        }
        return ExprP(new CallExpr(name, std::move(args), fd, rt));
      }
      // variable
      if (name == "gl_Position")
        return ExprP(new VarExpr(RK_BUILTIN, BI_POSITION, T_VEC4));
      if (name == "gl_PointSize")
        return ExprP(new VarExpr(RK_BUILTIN, BI_POINTSIZE, T_FLOAT));
      if (name == "gl_FragColor" || name == "gl_FragData")
        return ExprP(new VarExpr(RK_BUILTIN, BI_FRAGCOLOR, T_VEC4));
      if (name == "gl_FragCoord")
        return ExprP(new VarExpr(RK_BUILTIN, BI_FRAGCOORD, T_VEC4));
      if (name == "gl_FrontFacing")
        return ExprP(new VarExpr(RK_BUILTIN, BI_FRONTFACING, T_BOOL));
      RefInfo* r = Lookup(name);
      if (r == nullptr) {
        Fail("unknown identifier " + name);
        return ExprP(new NumExpr(0));
      }
      return ExprP(new VarExpr(r->kind, r->slot, r->type));
    }
    Fail("unexpected token");
    Advance();
    return ExprP(new NumExpr(0));
  }

  ExprP ParsePostfix() {
    ExprP e = ParsePrimary();
    for (;;) {
      if (Accept(".")) {
        std::string sw = tok.text;
        Advance();
        int map[4] = {0, 0, 0, 0};
        int n = (int)sw.size();
        bool ok = n >= 1 && n <= 4;
        for (int i = 0; i < n && ok; ++i) {
          switch (sw[i]) {
            case 'x': case 'r': case 's': map[i] = 0; break;
            case 'y': case 'g': case 't': map[i] = 1; break;
            case 'z': case 'b': case 'p': map[i] = 2; break;
            case 'w': case 'a': case 'q': map[i] = 3; break;
            default: ok = false;
          }
        }
        if (!ok) {
          Fail("bad swizzle ." + sw);
          return e;
        }
        e = ExprP(new SwizzleExpr(std::move(e), map, n));
        continue;
      }
      if (Accept("[")) {
        ExprP i = ParseExpr();
        Expect("]");
        e = ExprP(new IndexExpr(std::move(e), std::move(i)));
        continue;
      }
      if (Is("++") || Is("--")) {
        int d = Is("++") ? 1 : -1;
        Advance();
        e = ExprP(new IncDecExpr(std::move(e), d, true));
        continue;
      }
      break;
    }
    return e;
  }

  ExprP ParseUnary() {
    if (Accept("-"))
      return ExprP(new UnaryExpr(ParseUnary(), '-'));
    if (Accept("!"))
      return ExprP(new UnaryExpr(ParseUnary(), '!'));
    if (Accept("+"))
      return ParseUnary();
    if (Is("++") || Is("--")) {
      int d = Is("++") ? 1 : -1;
      Advance();
      return ExprP(new IncDecExpr(ParseUnary(), d, false));
    }
    return ParsePostfix();
  }

  ExprP ParseBin(int minprec) {
    ExprP lhs = ParseUnary();
    for (;;) {
      std::string op = tok.text;
      int prec = -1;
      if (op == "*" || op == "/") prec = 6;
      else if (op == "+" || op == "-") prec = 5;
      else if (op == "<" || op == ">" || op == "<=" || op == ">=") prec = 4;
      else if (op == "==" || op == "!=") prec = 3;
      else if (op == "&&") prec = 2;
      else if (op == "^^") prec = 1;
      else if (op == "||") prec = 0;
      if (prec < minprec)
        return lhs;
      Advance();
      ExprP rhs = ParseBin(prec + 1);
      lhs = ExprP(new BinExpr(std::move(lhs), std::move(rhs), op));
    }
  }

  ExprP ParseCond() {
    ExprP c = ParseBin(0);
    if (Accept("?")) {
      ExprP t = ParseAssign();
      Expect(":");
      ExprP f = ParseAssign();
      return ExprP(new CondExpr(std::move(c), std::move(t), std::move(f)));
    }
    return c;
  }

  ExprP ParseAssign() {
    ExprP lhs = ParseCond();
    std::string op = tok.text;
    if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=") {
      Advance();
      ExprP rhs = ParseAssign();
      char o = op == "=" ? '=' : op[0];
      return ExprP(new AssignExpr(std::move(lhs), std::move(rhs), o));
    }
    return lhs;
  }

  ExprP ParseExpr() {
    ExprP e = ParseAssign();
    while (Accept(","))
      e = ParseAssign();
    return e;
  }

  // -------- statements --------

  void SkipPrecision() {
    while (Is("highp") || Is("mediump") || Is("lowp"))
      Advance();
  }

  StmtP ParseDeclLocal(ValType ty) {
    auto blk = new BlockStmt();
    do {
      std::string name = tok.text;
      Advance();
      if (Accept("[")) {  // fixed-size local array: unsupported
        Fail("arrays not supported");
        return StmtP(blk);
      }
      int slot = frame_top++;
      if (frame_top > frame_max)
        frame_max = frame_top;
      scopes.back()[name] = {RK_LOCAL, slot, ty};
      ExprP init;
      if (Accept("="))
        init = ParseAssign();
      blk->body.emplace_back(new DeclStmt(slot, ty, std::move(init)));
    } while (Accept(","));
    Expect(";");
    return StmtP(blk);
  }

  StmtP ParseStmt() {
    if (Accept("{")) {
      auto blk = new BlockStmt();
      scopes.emplace_back();
      int save = frame_top;
      while (!Is("}") && !failed)
        blk->body.push_back(ParseStmt());
      Expect("}");
      scopes.pop_back();
      frame_top = save;
      return StmtP(blk);
    }
    if (Accept("if")) {
      auto st = new IfStmt();
      Expect("(");
      st->cond = ParseExpr();
      Expect(")");
      st->then_s = ParseStmt();
      if (Accept("else"))
        st->else_s = ParseStmt();
      return StmtP(st);
    }
    if (Accept("for")) {
      auto st = new ForStmt();
      scopes.emplace_back();
      int save = frame_top;
      Expect("(");
      if (!Accept(";")) {
        SkipPrecision();
        ValType ty;
        if (tok.kind == TK_IDENT && IsType(tok.text, &ty)) {
          Advance();
          SkipPrecision();
          st->init = ParseDeclLocal(ty);
        } else {
          st->init = StmtP(new ExprStmt(ParseExpr()));
          Expect(";");
        }
      }
      if (!Is(";"))
        st->cond = ParseExpr();
      Expect(";");
      if (!Is(")"))
        st->step = ParseExpr();
      Expect(")");
      st->body = ParseStmt();
      scopes.pop_back();
      frame_top = save;
      return StmtP(st);
    }
    if (Accept("while")) {
      auto st = new WhileStmt();
      Expect("(");
      st->cond = ParseExpr();
      Expect(")");
      st->body = ParseStmt();
      return StmtP(st);
    }
    if (Accept("return")) {
      auto st = new RetStmt();
      if (!Is(";"))
        st->e = ParseExpr();
      Expect(";");
      return StmtP(st);
    }
    if (Accept("break")) {
      Expect(";");
      return StmtP(new SimpleStmt(F_BREAK));
    }
    if (Accept("continue")) {
      Expect(";");
      return StmtP(new SimpleStmt(F_CONTINUE));
    }
    if (Accept("discard")) {
      Expect(";");
      return StmtP(new SimpleStmt(F_DISCARD));
    }
    if (Accept(";"))
      return StmtP(new BlockStmt());
    // declaration or expression
    SkipPrecision();
    ValType ty;
    if (tok.kind == TK_IDENT && IsType(tok.text, &ty)) {
      // lookahead: type ident => declaration; type '(' => constructor expr
      size_t save_p = lex.p;
      Tok save_t = tok;
      Advance();
      if (tok.kind == TK_IDENT) {
        SkipPrecision();
        return ParseDeclLocal(ty);
      }
      lex.p = save_p;
      tok = save_t;
    }
    ExprP e = ParseExpr();
    Expect(";");
    return StmtP(new ExprStmt(std::move(e)));
  }

  // -------- top level --------

  void ParseTop() {
    while (tok.kind != TK_END && !failed) {
      if (Accept("precision")) {
        while (!Is(";") && tok.kind != TK_END)
          Advance();
        Expect(";");
        continue;
      }
      if (Is("invariant")) {
        while (!Is(";") && tok.kind != TK_END)
          Advance();
        Expect(";");
        continue;
      }
      bool is_attr = false, is_uni = false, is_vary = false, is_const = false;
      for (;;) {
        if (Accept("attribute")) { is_attr = true; continue; }
        if (Accept("uniform")) { is_uni = true; continue; }
        if (Accept("varying")) { is_vary = true; continue; }
        if (Accept("const")) { is_const = true; continue; }
        if (Is("highp") || Is("mediump") || Is("lowp")) { Advance(); continue; }
        break;
      }
      ValType ty;
      if (tok.kind != TK_IDENT || !IsType(tok.text, &ty)) {
        Fail("expected type");
        return;
      }
      Advance();
      SkipPrecision();
      std::string name = tok.text;
      Advance();
      if (Is("(")) {
        ParseFunction(ty, name);
        continue;
      }
      // global-scope variable(s)
      for (;;) {
        if (Accept("[")) {
          Fail("arrays not supported");
          return;
        }
        if (is_attr) {
          int slot = FindVar(prog->attribs, name, ty, true);
          scopes[0][name] = {RK_ATTRIB, slot, ty};
        } else if (is_uni) {
          int slot = FindVar(prog->uniforms, name, ty, true);
          scopes[0][name] = {RK_UNIFORM, slot, ty};
        } else if (is_vary) {
          int slot = FindVar(prog->varyings, name, ty, true);
          scopes[0][name] = {RK_VARYING, slot, ty};
        } else {
          int slot = FindVar(prog->globals, name, ty, true);
          scopes[0][name] = {RK_GLOBAL, slot, ty};
          if (Is("=")) {
            Advance();
            ExprP init = ParseAssign();
            ginit->push_back({slot, init.get()});
            prog->owned_exprs.push_back(std::move(init));
          }
        }
        (void)is_const;
        if (Accept(","))  {
          name = tok.text;
          Advance();
          continue;
        }
        break;
      }
      Expect(";");
    }
  }

  void ParseFunction(ValType ret, const std::string& name) {
    FuncDef fd;
    fd.ret = ret;
    frame_top = 0;
    frame_max = 0;
    scopes.emplace_back();
    Expect("(");
    if (!Is(")")) {
      do {
        while (Is("in") || Is("out") || Is("inout") || Is("highp") ||
               Is("mediump") || Is("lowp") || Is("const"))
          Advance();
        ValType pt;
        if (tok.kind == TK_IDENT && IsType(tok.text, &pt)) {
          Advance();
          if (pt == T_VOID)
            break;
          SkipPrecision();
          std::string pn = tok.text;
          Advance();
          int slot = frame_top++;
          if (frame_top > frame_max)
            frame_max = frame_top;
          scopes.back()[pn] = {RK_LOCAL, slot, pt};
          fd.param_slots.push_back(slot);
          fd.param_types.push_back(pt);
        } else {
          Fail("expected parameter type");
          break;
        }
      } while (Accept(","));
    }
    Expect(")");
    Expect("{");
    while (!Is("}") && !failed)
      fd.body.push_back(ParseStmt());
    Expect("}");
    scopes.pop_back();
    fd.frame_size = frame_max > 0 ? frame_max : 1;
    (*funcs)[name] = std::move(fd);
  }
};

// ---- per-stage execution ---------------------------------------------------

bool RunMain(EvalCtx& ctx, const std::map<std::string, FuncDef>& funcs,
             const std::vector<std::pair<int, Expr*>>& ginit) {
  auto it = funcs.find("main");
  if (it == funcs.end())
    return false;
  for (const auto& gi : ginit)
    (*ctx.globals)[gi.first] = gi.second->Eval(ctx);
  ctx.frames.clear();
  ctx.frames.emplace_back(it->second.frame_size);
  bool discard = false;
  for (const auto& s : it->second.body) {
    Flow f = s->Exec(ctx);
    if (f == F_DISCARD) {
      discard = true;
      break;
    }
    if (f == F_RETURN)
      break;
  }
  return !discard;
}

// attribute fetch from a stream
Val FetchAttrib(const AttribStream& st, int vertex, ValType ty) {
  Val v;
  v.type = ty;
  v.f[3] = 1;
  if (!st.enabled || st.data == nullptr) {
    for (int i = 0; i < 4; ++i)
      v.f[i] = st.const_val[i];
    return v;
  }
  int comp_size = 4;
  switch (st.gltype) {
    case 0x1400: case 0x1401: comp_size = 1; break;  // BYTE/UBYTE
    case 0x1402: case 0x1403: comp_size = 2; break;  // SHORT/USHORT
    default: comp_size = 4; break;                   // FLOAT
  }
  int stride = st.stride != 0 ? st.stride : st.size * comp_size;
  const unsigned char* p = st.data + (size_t)stride * vertex;
  for (int i = 0; i < st.size && i < 4; ++i) {
    float x = 0;
    switch (st.gltype) {
      case 0x1406:  // FLOAT
        memcpy(&x, p + i * 4, 4);
        break;
      case 0x1401: {  // UNSIGNED_BYTE
        unsigned char b = p[i];
        x = st.normalized ? b / 255.f : (float)b;
        break;
      }
      case 0x1400: {  // BYTE
        signed char b = (signed char)p[i];
        x = st.normalized ? (b < 0 ? b / 128.f : b / 127.f) : (float)b;
        break;
      }
      case 0x1403: {  // UNSIGNED_SHORT
        unsigned short s;
        memcpy(&s, p + i * 2, 2);
        x = st.normalized ? s / 65535.f : (float)s;
        break;
      }
      case 0x1402: {  // SHORT
        short s;
        memcpy(&s, p + i * 2, 2);
        x = st.normalized ? s / 32767.f : (float)s;
        break;
      }
    }
    v.f[i] = x;
  }
  return v;
}

struct VtxOut {
  float pos[4];   // clip space
  std::vector<Val> varyings;
  bool valid = false;
};

}  // namespace

Program::~Program() = default;

Program* Compile(const std::string& vs_src, const std::string& fs_src,
                 std::string* log) {
  std::unique_ptr<Program> p(new Program());
  {
    Compiler c(p.get(), vs_src, false);
    c.ParseTop();
    if (c.failed) {
      if (log != nullptr)
        *log = "VS: " + c.err;
      return nullptr;
    }
    if (p->vs_funcs.find("main") == p->vs_funcs.end()) {
      if (log != nullptr)
        *log = "VS: no main";
      return nullptr;
    }
  }
  {
    Compiler c(p.get(), fs_src, true);
    c.ParseTop();
    if (c.failed) {
      if (log != nullptr)
        *log = "FS: " + c.err;
      return nullptr;
    }
    if (p->fs_funcs.find("main") == p->fs_funcs.end()) {
      if (log != nullptr)
        *log = "FS: no main";
      return nullptr;
    }
  }
  return p.release();
}

void Release(Program* p) {
  delete p;
}

const std::vector<VarInfo>& Attributes(const Program* p) {
  return p->attribs;
}
const std::vector<VarInfo>& Uniforms(const Program* p) {
  return p->uniforms;
}

bool Draw(Program* p, const DrawTarget& tgt, const DrawParams& dp) {
  if (tgt.color == nullptr || dp.count <= 0)
    return false;

  const int nvary = (int)p->varyings.size();
  std::vector<Val> globals(p->globals.size());
  std::vector<Val> attr_vals(p->attribs.size());
  std::vector<Val> vary_vals(nvary > 0 ? nvary : 1);

  EvalCtx ctx;
  ctx.prog = p;
  ctx.globals = &globals;
  ctx.uniforms = dp.uniforms;
  ctx.uniform_count = dp.uniform_count;
  ctx.attribs = attr_vals.data();
  ctx.varyings = vary_vals.data();
  ctx.dp = &dp;

  auto vertex_index = [&](int i) -> int {
    if (dp.indices == nullptr)
      return dp.first + i;
    if (dp.index_gltype == 0x1401)
      return dp.indices[i];
    if (dp.index_gltype == 0x1403) {
      unsigned short s;
      memcpy(&s, dp.indices + i * 2, 2);
      return s;
    }
    unsigned int u;
    memcpy(&u, dp.indices + i * 4, 4);
    return (int)u;
  };

  auto run_vertex = [&](int vi, VtxOut* out) {
    for (size_t a = 0; a < p->attribs.size(); ++a) {
      if ((int)a < dp.attrib_count)
        attr_vals[a] = FetchAttrib(dp.attribs[a], vi, p->attribs[a].type);
      else
        attr_vals[a] = Val();
      attr_vals[a].type = p->attribs[a].type;
    }
    for (int v = 0; v < nvary; ++v) {
      vary_vals[v] = Val();
      vary_vals[v].type = p->varyings[v].type;
    }
    ctx.fragment_stage = false;
    ctx.builtins[BI_POSITION] = Val();
    ctx.builtins[BI_POSITION].type = T_VEC4;
    ctx.builtins[BI_POINTSIZE] = MakeF(1);
    RunMain(ctx, p->vs_funcs, p->vs_ginit);
    memcpy(out->pos, ctx.builtins[BI_POSITION].f, sizeof(out->pos));
    out->varyings = vary_vals;
    out->valid = true;
  };

  const int vp_x = dp.viewport[0], vp_y = dp.viewport[1];
  const int vp_w = dp.viewport[2], vp_h = dp.viewport[3];

  auto to_screen = [&](const float pos[4], float* sx, float* sy, float* sz,
                       float* inv_w) -> bool {
    float w = pos[3];
    if (w < 1e-6f)
      return false;
    float ix = 1.f / w;
    float nx = pos[0] * ix, ny = pos[1] * ix, nz = pos[2] * ix;
    *sx = vp_x + (nx * 0.5f + 0.5f) * vp_w;
    // Row 0 of the target is the top; GL's +y is up.
    *sy = (tgt.h - vp_y) - (ny * 0.5f + 0.5f) * vp_h;
    *sz = nz * 0.5f + 0.5f;
    *inv_w = ix;
    return true;
  };

  // fragment varyings scratch
  std::vector<Val> frag_vary(nvary > 0 ? nvary : 1);

  auto shade_pixel = [&](int px, int py, float z, const Val* varys,
                         bool front) {
    if (dp.scissor) {
      int sy_top = tgt.h - (dp.scissor_box[1] + dp.scissor_box[3]);
      if (px < dp.scissor_box[0] ||
          px >= dp.scissor_box[0] + dp.scissor_box[2] || py < sy_top ||
          py >= sy_top + dp.scissor_box[3])
        return;
    }
    float* dpx = tgt.depth != nullptr ? &tgt.depth[(size_t)py * tgt.w + px]
                                      : nullptr;
    if (dp.depth_test && dpx != nullptr) {
      bool pass = true;
      switch (dp.depth_func) {
        case 0x0200: pass = false; break;             // NEVER
        case 0x0201: pass = z < *dpx; break;          // LESS
        case 0x0202: pass = z == *dpx; break;         // EQUAL
        case 0x0203: pass = z <= *dpx; break;         // LEQUAL
        case 0x0204: pass = z > *dpx; break;          // GREATER
        case 0x0205: pass = z != *dpx; break;         // NOTEQUAL
        case 0x0206: pass = z >= *dpx; break;         // GEQUAL
        case 0x0207: pass = true; break;              // ALWAYS
      }
      if (!pass)
        return;
    }
    for (int v = 0; v < nvary; ++v)
      vary_vals[v] = varys[v];
    ctx.fragment_stage = true;
    ctx.builtins[BI_FRAGCOLOR] = Val();
    ctx.builtins[BI_FRAGCOLOR].type = T_VEC4;
    Val& fc = ctx.builtins[BI_FRAGCOORD];
    fc.type = T_VEC4;
    fc.f[0] = px + 0.5f;
    fc.f[1] = (tgt.h - 1 - py) + 0.5f;
    fc.f[2] = z;
    fc.f[3] = 1;
    ctx.builtins[BI_FRONTFACING] = MakeF(front ? 1.f : 0.f);
    if (!RunMain(ctx, p->fs_funcs, p->fs_ginit))
      return;  // discard
    const float* col = ctx.builtins[BI_FRAGCOLOR].f;
    float out[4];
    for (int i = 0; i < 4; ++i) {
      float x = col[i];
      out[i] = x < 0 ? 0 : x > 1 ? 1 : x;
    }
    unsigned char* d = tgt.color + ((size_t)py * tgt.w + px) * 4;
    if (dp.blend) {
      float dst[4] = {d[0] / 255.f, d[1] / 255.f, d[2] / 255.f, d[3] / 255.f};
      auto factor = [&](int mode, bool for_src) -> float {
        switch (mode) {
          case 0: return 0;                       // ZERO
          case 1: return 1;                       // ONE
          case 0x0302: return out[3];             // SRC_ALPHA
          case 0x0303: return 1 - out[3];         // ONE_MINUS_SRC_ALPHA
          case 0x0304: return dst[3];             // DST_ALPHA
          case 0x0305: return 1 - dst[3];         // ONE_MINUS_DST_ALPHA
          default: return for_src ? 1.f : 0.f;
        }
      };
      float sf = factor(dp.blend_src, true);
      float df = factor(dp.blend_dst, false);
      for (int i = 0; i < 4; ++i) {
        float x = out[i] * sf + dst[i] * df;
        out[i] = x < 0 ? 0 : x > 1 ? 1 : x;
      }
    }
    for (int i = 0; i < 4; ++i)
      d[i] = (unsigned char)(out[i] * 255.f + 0.5f);
    if (dpx != nullptr && dp.depth_test && dp.depth_write)
      *dpx = z;
  };

  auto raster_tri = [&](const VtxOut& a, const VtxOut& b, const VtxOut& c) {
    float ax, ay, az, aw, bx, by, bz, bw, cx, cy, cz, cw;
    if (!to_screen(a.pos, &ax, &ay, &az, &aw) ||
        !to_screen(b.pos, &bx, &by, &bz, &bw) ||
        !to_screen(c.pos, &cx, &cy, &cz, &cw))
      return;
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (area == 0)
      return;
    // Screen y grows downward, so CCW-in-GL means negative screen area.
    bool front = dp.front_ccw ? area < 0 : area > 0;
    if (dp.cull) {
      if (dp.cull_face == 0x0408)  // FRONT_AND_BACK
        return;
      bool cull_front = dp.cull_face == 0x0404;
      if (front == cull_front)
        return;
    }
    int x0 = (int)floorf(min_f(ax, min_f(bx, cx)));
    int x1 = (int)ceilf(max_f(ax, max_f(bx, cx)));
    int y0 = (int)floorf(min_f(ay, min_f(by, cy)));
    int y1 = (int)ceilf(max_f(ay, max_f(by, cy)));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > tgt.w) x1 = tgt.w;
    if (y1 > tgt.h) y1 = tgt.h;
    float inv_area = 1.f / area;
    for (int py = y0; py < y1; ++py) {
      for (int px = x0; px < x1; ++px) {
        float fx = px + 0.5f, fy = py + 0.5f;
        float l0 = ((bx - fx) * (cy - fy) - (by - fy) * (cx - fx)) * inv_area;
        float l1 = ((cx - fx) * (ay - fy) - (cy - fy) * (ax - fx)) * inv_area;
        float l2 = 1.f - l0 - l1;
        if (l0 < 0 || l1 < 0 || l2 < 0)
          continue;
        float z = l0 * az + l1 * bz + l2 * cz;
        float wsum = l0 * aw + l1 * bw + l2 * cw;
        if (wsum == 0)
          continue;
        float k0 = l0 * aw / wsum, k1 = l1 * bw / wsum, k2 = l2 * cw / wsum;
        for (int v = 0; v < nvary; ++v) {
          Val& out = frag_vary[v];
          out.type = a.varyings[v].type;
          int n = Components(out.type);
          for (int i = 0; i < n; ++i)
            out.f[i] = k0 * a.varyings[v].f[i] + k1 * b.varyings[v].f[i] +
                       k2 * c.varyings[v].f[i];
        }
        shade_pixel(px, py, z, frag_vary.data(), front);
      }
    }
  };

  // vertex cache for indexed draws
  std::map<int, VtxOut> cache;
  auto vertex = [&](int i) -> const VtxOut& {
    int vi = vertex_index(i);
    auto it = cache.find(vi);
    if (it != cache.end())
      return it->second;
    VtxOut& out = cache[vi];
    run_vertex(vi, &out);
    return out;
  };

  switch (dp.mode) {
    case 0x0004:  // TRIANGLES
      for (int i = 0; i + 2 < dp.count; i += 3) {
        VtxOut a = vertex(i), b = vertex(i + 1), c = vertex(i + 2);
        raster_tri(a, b, c);
      }
      break;
    case 0x0005:  // TRIANGLE_STRIP
      for (int i = 0; i + 2 < dp.count; ++i) {
        VtxOut a = vertex(i), b = vertex(i + 1), c = vertex(i + 2);
        if (i & 1)
          raster_tri(b, a, c);
        else
          raster_tri(a, b, c);
      }
      break;
    case 0x0006:  // TRIANGLE_FAN
      for (int i = 1; i + 1 < dp.count; ++i) {
        VtxOut a = vertex(0), b = vertex(i), c = vertex(i + 1);
        raster_tri(a, b, c);
      }
      break;
    case 0x0000:  // POINTS
      for (int i = 0; i < dp.count; ++i) {
        VtxOut a = vertex(i);
        float sx, sy, sz, sw;
        if (!to_screen(a.pos, &sx, &sy, &sz, &sw))
          continue;
        int px = (int)sx, py = (int)sy;
        if (px >= 0 && px < tgt.w && py >= 0 && py < tgt.h)
          shade_pixel(px, py, sz, a.varyings.data(), true);
      }
      break;
    case 0x0001:  // LINES
    case 0x0003:  // LINE_STRIP
    {
      int step = dp.mode == 0x0001 ? 2 : 1;
      for (int i = 0; i + 1 < dp.count; i += step) {
        VtxOut a = vertex(i), b = vertex(i + 1);
        float ax, ay, az, aw, bx, by, bz, bw;
        if (!to_screen(a.pos, &ax, &ay, &az, &aw) ||
            !to_screen(b.pos, &bx, &by, &bz, &bw))
          continue;
        int steps = (int)max_f(fabsf(bx - ax), fabsf(by - ay)) + 1;
        for (int s = 0; s <= steps; ++s) {
          float t = steps > 0 ? (float)s / steps : 0;
          int px = (int)(ax + (bx - ax) * t);
          int py = (int)(ay + (by - ay) * t);
          if (px < 0 || px >= tgt.w || py < 0 || py >= tgt.h)
            continue;
          float z = az + (bz - az) * t;
          for (int v = 0; v < nvary; ++v) {
            Val& out = frag_vary[v];
            out.type = a.varyings[v].type;
            int n = Components(out.type);
            for (int k = 0; k < n; ++k)
              out.f[k] = a.varyings[v].f[k] +
                         (b.varyings[v].f[k] - a.varyings[v].f[k]) * t;
          }
          shade_pixel(px, py, z, frag_vary.data(), true);
        }
      }
      break;
    }
    default:
      return false;
  }
  return true;
}

}  // namespace brgl

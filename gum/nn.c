// nn.c: gum.bin's forward pass (export.py). a tensor is c planes of h × w
// floats. a dense convolution is a gemm of its packed weights by its packed
// im2col, a depthwise one direct; the rest is elementwise
#include <math.h>
#ifdef __SSE__
#include <xmmintrin.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float *p; int c, h, w; } ten;
typedef struct {
  int k, n, in[8], co, ci, kh, kw, s, p, g, act, last;
  const float *w, *sc, *sh;   // weights, then x · sc + sh a channel
  float *a;   // the weights packed for the gemm, MR rows at a time
} op;
typedef struct { op *o; int n, nl; double *xp, *fp; char *buf; } net;

#define MR 8
#define NR 8
#define KC 256
#define MC 128
typedef float vec __attribute__((vector_size(4 * NR)));

static const char *load(net *m, const char *path) {
  FILE *f = fopen(path, "rb");
  long n;
  if (!f) return "cannot open the model";
  fseek(f, 0, SEEK_END), n = ftell(f), rewind(f);
  char *b = m->buf = malloc(n), *q = b + 4;
  if (fread(b, 1, n, f) != (size_t)n || memcmp(b, "gum1", 4))
    return fclose(f), "not a gum model";
  fclose(f);
#define I(v) (memcpy(&(v), q, 4), q += 4)
  I(m->n), m->o = calloc(m->n + 1, sizeof *m->o);
  for (op *o = m->o + 1; o <= m->o + m->n; o++) {
    I(o->k), I(o->n);
    for (int i = 0; i < o->n; i++) I(o->in[i]), m->o[o->in[i]].last = o - m->o;
    if (o->k != 'C') continue;
    I(o->co), I(o->ci), I(o->kh), I(o->kw), I(o->s), I(o->p), I(o->g);
    I(o->act);
    int K = o->ci * o->kh * o->kw, M = (o->co + MR - 1) / MR * MR;
    o->w = (float *)q, q += 4 * (size_t)o->co * K;
    o->sc = (float *)q, q += 4 * o->co;
    o->sh = (float *)q, q += 4 * o->co;
    if (o->g > 1) continue;
    o->a = calloc((size_t)M * K, 4);
    for (int i = 0; i < o->co; i++)
      for (int k = 0; k < K; k++)
        o->a[(size_t)(i / MR) * MR * K + k * MR + i % MR] = o->w[i * K + k];
  }
  I(m->nl), m->xp = (double *)q, m->fp = m->xp + m->nl;
  return q + 16 * m->nl == b + n ? NULL : "truncated model";
}

// c[MR × NR] (row stride ldc, m × n of it) = or += a[k][MR] · b[k][NR]
// (row stride ldb)
static void kern(int k, const float *a, const float *b, int ldb, float *c,
                 int ldc, int m, int n, int first) {
  vec s[MR] = {0}, v;
  for (int p = 0; p < k; p++, a += MR, b += ldb) {
    memcpy(&v, b, sizeof v);
    for (int i = 0; i < MR; i++) s[i] += a[i] * v;
  }
  for (int i = 0; i < m; i++, c += ldc)
    if (n == NR) {
      if (!first) memcpy(&v, c, sizeof v), s[i] += v;
      memcpy(c, &s[i], sizeof v);
    } else
      for (int j = 0; j < n; j++) c[j] = s[i][j] + (first ? 0 : c[j]);
}

// panels [j0, j1) of the im2col of x for o, each k × NR, into b: a run of
// NR outputs along one row, a row of an input plane, wherever it can be.
// t: each k's plane offset, ky and kx
static void pack(const op *o, const ten *x, int ow, int on, int j0, int j1,
                 const int (*t)[3], float *b) {
  int K = o->ci * o->kh * o->kw, oy[NR], ox[NR];
  for (int j = j0; j < j1; j++, b += (size_t)K * NR) {
    int n = on - j * NR < NR ? on - j * NR : NR;
    for (int i = 0; i < NR; i++)
      oy[i] = (j * NR + i) / ow * o->s - o->p,
      ox[i] = (j * NR + i) % ow * o->s - o->p;
    int run = n == NR && o->s == 1 && oy[0] == oy[NR - 1];
    for (int k = 0; k < K; k++) {
      int y = oy[0] + t[k][1], x0 = ox[0] + t[k][2];
      const float *xc = x->p + t[k][0];
      float *d = b + (size_t)k * NR;
      if (run && y >= 0 && y < x->h && x0 >= 0 && x0 + NR <= x->w)
        memcpy(d, xc + (size_t)y * x->w + x0, 4 * NR);
      else if (run) {   // what of the run lies in the plane, zeros about it
        int a = x0 < 0 ? -x0 : 0;
        int z = y < 0 || y >= x->h ? 0 : x->w - x0 < NR ? x->w - x0 : NR;
        for (int i = 0; i < NR; i++)
          d[i] = i >= a && i < z ? xc[(size_t)y * x->w + x0 + i] : 0;
      } else
        for (int i = 0; i < NR; i++) {
          int yy = oy[i] + t[k][1], xx = ox[i] + t[k][2];
          d[i] = i < n && yy >= 0 && yy < x->h && xx >= 0 && xx < x->w
               ? xc[(size_t)yy * x->w + xx] : 0;
        }
    }
  }
}

// expf without branches, so that loops of it vectorise: cephes' to 2 ulp
static inline float ex(float x) {
  x = x < -87.3f ? -87.3f : x > 88.3f ? 88.3f : x;
  float n = floorf(x * 1.44269504f + .5f), s;
  float r = x - n * .693359375f + n * 2.12194440e-4f;
  float p = ((((1.9875691500e-4f * r + 1.3981999507e-3f) * r +
               8.3334519073e-3f) * r + 4.1665795894e-2f) * r +
             1.6666665459e-1f) * r + 5.0000001201e-1f;
  int32_t e = ((int32_t)n + 127) << 23;
  return memcpy(&s, &e, 4), (p * r * r + r + 1) * s;
}

// none, relu, silu (x · sigmoid x, as timm has it) or sigmoid
static inline float act(int a, float v) {
  return a == 1 ? (v > 0 ? v : 0) : a == 2 ? v * (1 / (1 + ex(-v)))
       : a == 3 ? 1 / (1 + ex(-v)) : v;
}

static void conv(const op *o, const ten *x, ten *y) {
  int oh = y->h = (x->h + 2 * o->p - o->kh) / o->s + 1, M = o->co;
  int ow = y->w = (x->w + 2 * o->p - o->kw) / o->s + 1, N = oh * ow;
  int K = o->ci * o->kh * o->kw, np = (N + NR - 1) / NR;
  y->c = M, y->p = malloc(4 * (size_t)M * N);
  if (o->g > 1) {   // depthwise, tap by tap along rows of each plane
    int pw = x->w + 2 * o->p, ph = x->h + 2 * o->p;   // padded with zeros
    float *q = calloc((size_t)pw * ph, 4);
    for (int c = 0; c < M; c++) {
      float *yc = y->p + (size_t)c * N;
      for (int r = 0; r < x->h; r++)
        memcpy(q + (size_t)(r + o->p) * pw + o->p,
               x->p + ((size_t)c * x->h + r) * x->w, 4 * x->w);
      for (int r = 0; r < oh; r++) {
        float *yr = yc + (size_t)r * ow;
        memset(yr, 0, 4 * ow);
        for (int ky = 0; ky < o->kh; ky++)
          for (int kx = 0; kx < o->kw; kx++) {
            float w = o->w[(c * o->kh + ky) * o->kw + kx];
            const float *xr = q + (size_t)(r * o->s + ky) * pw + kx;
            for (int i = 0; i < ow; i++) yr[i] += w * xr[i * o->s];
          }
        for (int i = 0; i < ow; i++)
          yr[i] = act(o->act, yr[i] * o->sc[c] + o->sh[c]);
      }
    }
    return free(q);
  }
  // panels in chunks of about 2 MB of im2col; each k-block of a, in cache,
  // runs over a chunk's panels. a pointwise one reads x as it is
  int nb = (2 << 20) / (4 * NR * K), d = K == o->ci && o->s == 1 &&
                                         !o->p && N % NR == 0;
  nb = nb < 1 ? 1 : nb;
  float *b = malloc(4 * (size_t)K * NR * (nb < np ? nb : np));
  int (*t)[3] = malloc(sizeof *t * K), hw = o->kh * o->kw;
  for (int k = 0; k < K; k++)
    t[k][0] = k / hw * x->h * x->w, t[k][1] = k % hw / o->kw,
    t[k][2] = k % o->kw;
  for (int j0 = 0; j0 < np; j0 += nb) {
    int j1 = j0 + nb < np ? j0 + nb : np;
    if (!d) pack(o, x, ow, N, j0, j1, (const int (*)[3])t, b);
    for (int k0 = 0; k0 < K; k0 += KC)   // a's MC × KC blocks in cache
      for (int i0 = 0, kc = K - k0 < KC ? K - k0 : KC; i0 < M; i0 += MC)
        for (int j = j0; j < j1; j++)
          for (int i = i0; i < M && i < i0 + MC; i += MR)
            kern(kc, o->a + (size_t)i * K + k0 * MR,
                 d ? x->p + (size_t)k0 * N + j * NR
                   : b + (size_t)(j - j0) * K * NR + k0 * NR, d ? N : NR,
                 y->p + (size_t)i * N + j * NR, N, M - i < MR ? M - i : MR,
                 N - j * NR < NR ? N - j * NR : NR, !k0);
  }
  free(b), free(t);
  for (int c = 0; c < M; c++)
    for (float *p = y->p + (size_t)c * N, *e = p + N; p < e; p++)
      *p = act(o->act, *p * o->sc[c] + o->sh[c]);
}

// the forward pass over x (24 planes, sides a multiple of 32): the output's
// two planes, the likelihood logit and the conditional, in a new tensor
static ten forward(const net *m, ten x) {
  ten *v = calloc(m->n + 1, sizeof *v), y;
#ifdef __SSE__   // subnormals flushed: x86 takes ~100 cycles over each one,
  _mm_setcsr(_mm_getcsr() | 0x8040);   // and they are below 1e-38
#endif
  v[0] = x;
  for (int i = 1; i <= m->n; i++) {
    const op *o = m->o + i;
    ten *a = v + o->in[0], *b = v + o->in[1], *r = v + i;
    size_t n = (size_t)a->h * a->w, N = a->c * n;
    *r = (ten){NULL, a->c, a->h, a->w};
    if (o->k == 'C') conv(o, a, r);
    else if (o->k == 'G') {
      r->h = r->w = 1, r->p = malloc(4 * a->c);
      for (int c = 0; c < a->c; c++) {
        double s = 0;
        for (size_t j = 0; j < n; j++) s += a->p[c * n + j];
        r->p[c] = s / n;
      }
    } else if (o->k == 'U') {
      r->h *= 2, r->w *= 2, r->p = malloc(16 * N);
      for (int c = 0; c < a->c; c++)
        for (int y = 0; y < r->h; y++)
          for (int x = 0; x < r->w; x++)
            r->p[(c * r->h + y) * (size_t)r->w + x] =
              a->p[(c * a->h + y / 2) * (size_t)a->w + x / 2];
    } else if (o->k == 'K') {
      r->c = 0;
      for (int j = 0; j < o->n; j++) r->c += v[o->in[j]].c;
      float *p = r->p = malloc(4 * n * r->c);
      for (int j = 0; j < o->n; j++)
        memcpy(p, v[o->in[j]].p, 4 * n * v[o->in[j]].c),
        p += n * v[o->in[j]].c;
    } else {   // A, M: b alike or one value a plane
      size_t bn = (size_t)b->h * b->w;
      r->p = malloc(4 * N);
      for (size_t c = 0; c < N; c += n)
        for (size_t j = c; j < c + n; j++) {
          float q = bn == 1 ? b->p[c / n] : b->p[j];
          r->p[j] = o->k == 'A' ? a->p[j] + q : a->p[j] * q;
        }
    }
    for (int j = 0; j < o->n; j++)
      if (m->o[o->in[j]].last == i && o->in[j])
        free(v[o->in[j]].p), v[o->in[j]].p = NULL;
  }
  y = v[m->n], free(v);
  return y;
}

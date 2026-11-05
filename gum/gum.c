// gum: orbio's project eucalyptus sentinel-2 methane plume detector, read
// straight off the l1c jp2 archive. jobs come on stdin, one per line:
//   key \t x \t y \t b12
// the centre of a 5.12 km box in metres of the scene's UTM zone and the
// url of its B12 jp2 (s3:// on eodata, signed with AWS_ACCESS_KEY_ID,
// AWS_SECRET_ACCESS_KEY and AWS_S3_ENDPOINT; https:// or file://). the
// lines of a key are one site's time series on one MGRS tile, oldest
// first: each scene is the target of the model with the two latest
// earlier scenes whose box is at most 5% cloud or no data as references,
// as notebooks/sentinel2.ipynb's stack. one line of json out per job.
//   gum MODEL < jobs.tsv > out.jsonl
//   gum MODEL B12 X Y > crop.u16   (one scene's 8 × 256 × 256 crop)
//
// the box's pixels are read as the notebook's stack has them: 20 m bands
// at full resolution and 10 m bands at the jp2's first reduced resolution
// (what gdal gives a 2× read), less the 1000 DN offset from baseline 04.00.
// a scene's reads go at once, round by round: the headers and the cloud
// mask (MSK_CLASSI), then each band's tile-parts in the box, located by the
// TLM, or without one by SOT markers near where the key's last date had
// them. after the model (nn.c), the notebook's watershed masks and
// major-axis quantification, the wind left out: q is kg/h per m/s.
#define _GNU_SOURCE
#include <curl/curl.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include "j2k.c"
#include "nn.c"

#define S 256
#define NB 8
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
static const char *BAND[NB] = {"B11", "B12", "B8A", "B07", "B05", "B04",
                               "B03", "B02"};
static const float MARK = .3044f, FLOOR = .1522f, CLEAR = .05f;

typedef struct { unsigned char *p; size_t n, cap; long long size; int wait; }
  buf;

static size_t wr(char *d, size_t s, size_t n, void *u) {
  buf *b = u;
  if (b->n + s * n > b->cap) b->p = realloc(b->p, b->cap = 2 * (b->n + s * n));
  return memcpy(b->p + b->n, d, s * n), b->n += s * n, s * n;
}

static size_t hd(char *d, size_t s, size_t n, void *u) {
  buf *b = u;
  char *sl = memchr(d, '/', s * n);
  if (!strncasecmp(d, "content-range:", 14) && sl) b->size = atoll(sl + 1);
  if (!strncasecmp(d, "retry-after:", 12)) b->wait = atoi(d + 12);
  return s * n;
}

// a read of [lo, hi) of url, hi -1 to the end
typedef struct { const char *url; long long lo, hi; buf b; const char *e;
                 int ok, tries; } rd;
typedef struct {   // a worker's connections, and its requests and bytes
  CURL *c;
  CURLM *m;
  char *cred, sig[64];
  long reads, bytes;
} ctx;

static void open_(ctx *x) {
  const char *id = getenv("AWS_ACCESS_KEY_ID"),
             *key = getenv("AWS_SECRET_ACCESS_KEY"), *rg = getenv("AWS_REGION");
  *x = (ctx){curl_easy_init(), curl_multi_init(), NULL, "", 0, 0};
  curl_easy_setopt(x->c, CURLOPT_WRITEFUNCTION, wr);
  curl_easy_setopt(x->c, CURLOPT_HEADERFUNCTION, hd);
  curl_easy_setopt(x->c, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(x->c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(x->c, CURLOPT_LOW_SPEED_TIME, 30L);
  curl_multi_setopt(x->m, CURLMOPT_MAX_HOST_CONNECTIONS, 16L);
  snprintf(x->sig, sizeof x->sig, "aws:amz:%s:s3", rg ? rg : "us-east-1");
  if (id && key && asprintf(&x->cred, "%s:%s", id, key) < 0) x->cred = NULL;
}

// the reads of q[0, n) not done, at once, until each is done or failed:
// network errors, 429s and 5xx retry, after the wait a 429 names or a
// backoff
static void fetch(ctx *x, rd *q, int n) {
  for (int left = 1, k; left;) {
    int wait = 0;
    CURLMsg *g;
    for (rd *r = q; r < q + n; r++)
      if (!r->ok && !r->e && r->tries) wait = MAX(wait, r->b.wait);
    if (wait) sleep(wait);
    for (rd *r = q; r < q + n; r++) {
      if (r->ok || r->e) continue;
      CURL *c = curl_easy_duphandle(x->c);
      char u[4096], range[48];
      const char *s3 = getenv("AWS_S3_ENDPOINT");
      if (!strncmp(r->url, "s3://", 5))
        snprintf(u, sizeof u, "https://%s/%s",
                 s3 ? s3 : "eodata.dataspace.copernicus.eu", r->url + 5);
      else if (!strncmp(r->url, "gs://", 5))
        snprintf(u, sizeof u, "https://storage.googleapis.com/%s", r->url + 5);
      else snprintf(u, sizeof u, "%s", r->url);
      snprintf(range, sizeof range, r->hi < 0 ? "%lld-" : "%lld-%lld", r->lo,
               r->hi - 1);
      curl_easy_setopt(c, CURLOPT_URL, u);
      if (x->cred && !strncmp(r->url, "s3://", 5))
        curl_easy_setopt(c, CURLOPT_AWS_SIGV4, x->sig),
        curl_easy_setopt(c, CURLOPT_USERPWD, x->cred);
      curl_easy_setopt(c, CURLOPT_RANGE, range);
      curl_easy_setopt(c, CURLOPT_WRITEDATA, &r->b);
      curl_easy_setopt(c, CURLOPT_HEADERDATA, &r->b);
      curl_easy_setopt(c, CURLOPT_PRIVATE, r);
      r->b.n = 0, r->b.size = -1, r->b.wait = 0, x->reads++;
      curl_multi_add_handle(x->m, c);
    }
    for (k = 1; k;)
      if (!curl_multi_perform(x->m, &k) && k)
        curl_multi_poll(x->m, NULL, 0, 1000, NULL);
    while ((g = curl_multi_info_read(x->m, &k))) {
      rd *r;
      long code = 0;
      curl_easy_getinfo(g->easy_handle, CURLINFO_PRIVATE, (char **)&r);
      curl_easy_getinfo(g->easy_handle, CURLINFO_RESPONSE_CODE, &code);
      CURLcode e = g->data.result;
      struct stat st;
      if (e == CURLE_OK && !strncmp(r->url, "file://", 7))
        code = 206, r->b.size = stat(r->url + 7, &st) ? -1 : st.st_size;
      if (e == CURLE_FILE_COULDNT_READ_FILE || e == CURLE_URL_MALFORMAT ||
          e == CURLE_UNSUPPORTED_PROTOCOL || e == CURLE_LOGIN_DENIED)
        r->e = curl_easy_strerror(e);
      else if (e == CURLE_OK && code < 300) r->ok = 1, x->bytes += r->b.n;
      else if (e == CURLE_OK && code != 429 && code < 500)
        r->e = code == 404 ? "not found" : "http error";
      else if (++r->tries == 8) r->e = "fetch failed after retries";
      else r->b.wait = code == 429 && r->b.wait > 0 ? MIN(r->b.wait, 60)
                                                    : 1 << MIN(r->tries, 5);
      curl_multi_remove_handle(x->m, g->easy_handle);
      curl_easy_cleanup(g->easy_handle);
    }
    for (left = 0, k = 0; k < n; k++) left += !q[k].ok && !q[k].e;
  }
}

static unsigned be(const unsigned char *p, int n) {
  unsigned v = 0;
  while (n--) v = v << 8 | *p++;
  return v;
}

// a band's file: its header, where its tile-parts are (0 unknown), and
// the reads made of it; the box's corner in its pixels, at resolution
// nl - red
typedef struct {
  char *u;
  j2k j;
  double ox, oy, res;
  long long size, first, at[256], len[256];
  int tlm, nr, x0, y0, miss;   // miss: rounds that found no tile-part
  rd r[64];
} band;

// where the last file read of each MGRS tile and band had its tile-parts,
// for the workers to share: that file's exactly, another date's near
typedef struct { char key[16], *url; long long size, at[256], len[256]; } lay;
static lay lays[4096];
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;

// f's slot (T31SGR_B02 of .../T31SGR_20191120T101321_B02.jp2), and with
// put, f's tile-parts into it, else its into f and a copy into l
static void share(band *f, lay *l, int put) {
  const char *n = strrchr(f->u, '/') ? strrchr(f->u, '/') + 1 : f->u;
  char key[16];
  unsigned long h = 5381;
  snprintf(key, sizeof key, "%.6s%.4s", n, strrchr(n, '_'));
  for (char *c = key; *c; c++) h = h * 33 + *c;
  pthread_mutex_lock(&lk);
  lay *s = &lays[h % 4096];
  int same = !strcmp(s->key, key) && s->url && !strcmp(s->url, f->u);
  if (put) {
    if (!same) free(s->url), *s = (lay){"", strdup(f->u), f->size, {0}, {0}};
    strcpy(s->key, key);
    for (int k = 0; k < f->j.nt; k++)
      if (f->len[k]) s->at[k] = f->at[k], s->len[k] = f->len[k];
  } else if (!strcmp(s->key, key)) {
    *l = *s;
    for (int k = 0; same && k < f->j.nt; k++)
      if (s->len[k] && !f->len[k]) f->at[k] = s->at[k], f->len[k] = s->len[k];
  }
  pthread_mutex_unlock(&lk);
}

// the bytes of f from lo on that one read holds, as many as any does
static const unsigned char *from(const band *f, long long lo, long long *n) {
  const unsigned char *p = NULL;
  *n = 0;
  for (const rd *r = f->r; r < f->r + f->nr; r++)
    if (r->ok && r->lo <= lo && r->lo + (long long)r->b.n - lo > *n)
      *n = r->lo + r->b.n - lo, p = r->b.p + (lo - r->lo);
  return p;
}

// how much of tile-part k from its start the box needs: to the end of the
// last packet it reaches (with one layer, LRCP puts the lower resolutions
// first), by the PLT in its header; all of it without a PLT; 0 while the
// header is not held
static long long upto(const band *f, int k) {
  long long n, end = 0;
  const unsigned char *p = from(f, f->at[k], &n);
  uint32_t o[4096], l[4096];
  char c[4096];
  size_t len;
  int t;
  if (!p || f->j.mp > 4096) return 0;
  if (j2k_part(&f->j, p, MIN(n, f->len[k]), &t, &len, o, l)) return f->len[k];
  for (int i = 0, m = j2k_need(&f->j, k, f->x0, f->y0, f->x0 + S, f->y0 + S,
                                c); i < m; i++)
    if (c[i]) end = MAX(end, (long long)o[i] + l[i]);
  return end;
}

// a tile-part header at p, of a file of size z: SOT, Lsot = 10, one part
static int sot(const unsigned char *p, const band *f, int *k, long long *l) {
  *k = be(p + 4, 2), *l = be(p + 6, 4);
  return be(p, 2) == 0xff90 && be(p + 2, 2) == 10 && *k < f->j.nt && !p[10] &&
         p[11] == 1 && *l >= 14;
}

// f's tile-parts in reads not yet scanned, by their SOT markers, each one
// confirmed by the next where the read holds it. a round that finds none
// widens the next one's windows
static void scan(band *f, int from) {
  int found = 0;
  for (rd *r = f->r + from; r < f->r + f->nr; r++) {
    const unsigned char *p = r->b.p, *z = p + r->b.n;
    for (int k, j; r->ok && (p = memchr(p, 0xff, z - p)) && z - p >= 12; p++) {
      long long l, m, at = r->lo + (p - r->b.p);
      if (!sot(p, f, &k, &l) || at + l > f->size ||
          (p + l + 12 <= z && at + l < f->size &&
           (!sot(p + l, f, &j, &m) || j != k + 1)))
        continue;
      found += !f->len[k], f->at[k] = at, f->len[k] = l;
      if (k + 1 < f->j.nt && !f->at[k + 1]) f->at[k + 1] = at + l;
    }
  }
  f->miss = from < f->nr && !found ? MIN(f->miss + 1, 6) : 0;
}

// the tiles that f's box meets, as a mask, at resolution nl - red
static void tiles(const band *f, char *need) {
  for (int k = 0; k < f->j.nt; k++) {
    j2k_box t = rbox(tbox(&f->j, k), f->j.red);
    need[k] = t.x0 < f->x0 + S && f->x0 < t.x1 && t.y0 < f->y0 + S &&
              f->y0 < t.y1;
  }
}

// the next reads f's tile-parts in the box need, appended to q, runs of
// them as one: of each one located, what its PLT says the box needs; of
// each one not, a window where it should be
static int plan(band *f, const lay *L, rd *q) {
  char need[256];
  int n = 0;
  long long avg = (f->size - f->first) / f->j.nt;
  tiles(f, need);
  for (int k = 0; k < f->j.nt; k++) {
    long long a = f->at[k], l = f->len[k], lo, hi;
    long long u = need[k] && a && l ? upto(f, k) : 0, h;
    if (!need[k] || (u && from(f, a, &h) && u <= h)) continue;
    // a 10 m band at 20 m reads first what its lower levels should need
    if (a && l) lo = a, hi = a + (u ? u : f->j.red ? MIN(l, l * 2 / 3) : l);
    else {   // a window where it should be: from the nearer tile known,
      // past the lengths between, as the layout has them or the mean of
      // those known
      int i = k, j = k, c = 0;
      long long e = 0, g = 0;
      while (--i >= 0 && !f->len[i]);
      while (++j < f->j.nt && !f->at[j]);
      for (int t = 0; t < f->j.nt; t++) c += !!f->len[t], e += f->len[t];
      e = c ? e / c : avg;
#define EST(t) (L->len[t] > 0 ? (double)L->len[t] / L->size * f->size : e)
      long long B = a ? a : i < 0 ? f->first : f->at[i] + f->len[i];
      long long A = j < f->j.nt ? f->at[j] : f->size;
      int d = MIN(k - i - 1, j - k);
      int up = k - i - 1 <= j - k;
      for (int t = up ? i + 1 : k; t < (up ? k : j); t++) g += EST(t);
      g = a ? a : up ? B + g : A - g;
      long long w = MIN(A - B, (long long)(EST(k) * 1.25 + sqrt(d) * e / 2 +
                                           (64 << 10)) << f->miss);
      lo = MAX(B, MIN(g - w / 4, A - w)), hi = MIN(lo + w, A);
    }
    if (n && q[n - 1].hi >= lo && q[n - 1].lo <= lo)
      q[n - 1].hi = MAX(q[n - 1].hi, hi);
    else q[n++] = (rd){f->u, lo, hi, {0}, NULL, 0, 0};
  }
  return n;
}

// f's header from the read r: its georeferencing (GMLJP2), its coding and
// its TLM, if it has one
static const char *header(band *f, rd *r) {
  const unsigned char *p = r->b.p, *e = p + r->b.n, *s, *t, *q;
  if (!r->ok) return r->e;
  f->size = r->b.size;
  if (f->size <= 0) return "unknown file size";
  s = memmem(p, r->b.n, "\xff\x4f\xff\x51", 4);
  for (t = NULL, q = s ? s + 2 : NULL; q && !t && q + 4 <= e;
       q += 2 + be(q + 2, 2))
    if (be(q, 2) == 0xff90) t = q;
  if (!t) return "no codestream header in the first 64 KB";
  const char *g = memmem(p, s - p, "<gml:pos>", 9);
  const char *o = g ? memmem(g, (const char *)s - g, "<gml:offsetVector", 17)
                    : NULL;
  if (!o || !(o = memchr(o, '>', 128)) ||
      sscanf(g + 9, "%lf %lf", &f->ox, &f->oy) != 2 ||
      sscanf(o + 1, "%lf", &f->res) != 1 || f->res <= 0)
    return "no GMLJP2 georeferencing";
  f->ox -= f->res / 2, f->oy += f->res / 2;
  const char *err = j2k_main(&f->j, s, t - s);
  if (err) return err;
  if (f->j.nt > 256) return "more than 256 jp2 tiles";
  memset(f->at, 0, sizeof f->at), memset(f->len, 0, sizeof f->len);
  f->first = f->at[0] = t - p, f->tlm = 0;
  for (q = s + 2; q + 4 <= t; q += 2 + be(q + 2, 2)) {
    int L = be(q + 2, 2), st = q[5] >> 4 & 3, sp = q[5] & 64 ? 4 : 2;
    long long at = f->first;
    if (be(q, 2) != 0xff55) continue;
    for (int i = 6, n = 0; i + st + sp <= L + 2; i += st + sp, n++) {
      int k = st ? (int)be(q + i, st) : n;
      if (k < f->j.nt) f->at[k] = at, f->len[k] = be(q + i + st, sp);
      at += be(q + i + st, sp), f->tlm = 1;
    }
  }
  return NULL;
}

static int fd(int a, int b) { return a >= 0 ? a / b : -((b - 1 - a) / b); }

static char *swap(const char *u, const char *from, const char *to) {
  const char *p = strstr(u, from);
  size_t n = strlen(u) + strlen(to) + 1;
  char *r = p ? malloc(n) : NULL;
  if (r) snprintf(r, n, "%.*s%s", (int)(p - u), u, to);
  return r;
}

// one scene's box: its 8 bands, harmonised, the corner of the box in UTM
// metres, and its share of cloud (the mask's opaque or cirrus) or no data
// (B12 0). with the bands wanted only if it is clear, a box over 5% cloud
// ends at its mask
typedef struct { uint16_t v[NB][S * S]; double X, Y; float bad; int row; }
  crop;

static const char *scene(ctx *x, const char *b12, double cx, double cy,
                         int mask, int bands, crop *o) {
  band *f = calloc(NB, sizeof *f);
  rd q[NB * 16 + 1], *m = q + NB;
  lay *L = calloc(NB, sizeof *L);
  const char *nb = strstr(b12, "_N"), *e = NULL;
  int off = nb && atoi(nb + 2) >= 400 ? 1000 : 0, bad = 0;
  char t[16], *mu = swap(b12, "IMG_DATA/", "QI_DATA/MSK_CLASSI_B00.jp2");
  uint8_t *cl = calloc(S * S, 1);
  o->bad = 0;
  if (!strstr(b12, "_B12.jp2") || !mu) e = "b12 url not in SAFE form";
  else if (mask && !off) e = "needs baseline N0400 or later";
  for (int b = 0; b < NB && !e; b++)
    snprintf(t, sizeof t, "_%s.jp2", BAND[b]),
    f[b].u = swap(b12, "_B12.jp2", t),
    q[b] = (rd){f[b].u, 0, 64 << 10, {0}, NULL, 0, 0};
  *m = (rd){mu, 0, -1, {0}, NULL, 0, 0};
  if (!e) fetch(x, q, NB + mask);
  for (int b = 0; b < NB && !e; b++) {   // the box in each band's pixels
    band *g = &f[b];
    if ((e = header(g, &q[b]))) break;
    g->r[g->nr++] = q[b], q[b].b = (buf){0};
    g->j.red = g->res < 15;   // 10 m bands at 20 m
    share(g, &L[b], 0);
    g->x0 = (int)floor((cx - S * 10 - g->ox) / 20 + .5);
    g->y0 = (int)floor((g->oy - cy - S * 10) / 20 + .5);
  }
  o->X = f[1].ox + f[1].x0 * 20., o->Y = f[1].oy - f[1].y0 * 20.;
  if (!e && mask && !(e = m->ok ? NULL : m->e)) {   // 60 m cells
    int sx = f[1].x0, sy = f[1].y0, x0 = fd(sx, 3), y0 = fd(sy, 3);
    int w = fd(sx + S - 1, 3) - x0 + 1, h = fd(sy + S - 1, 3) - y0 + 1;
    uint16_t *c = calloc(2 * w * h, 2);
    e = j2k_window(m->b.p, m->b.n, x0, y0, x0 + w, y0 + h, 2, c);
    for (int i = 0, j; i < S * S; i++)
      j = (fd(sy + i / S, 3) - y0) * w + fd(sx + i % S, 3) - x0,
      bad += cl[i] = c[j] || c[w * h + j];
    o->bad = (float)bad / (S * S), free(c);
  }
  free(m->b.p), free(mu);
  for (int round = 0, n = !e && (bands || o->bad <= CLEAR); n; round++) {
    int at[NB + 1];   // the tile-parts, round by round
    n = 0;
    for (int b = 0; b < NB; b++)   // a band on another's grid that has
      for (int c = 0; c < NB; c++) {   // found more tile-parts follows it
        int m = 0;
        for (int k = 0; f[c].j.tw == f[b].j.tw && k < f[b].j.nt; k++)
          m += !!f[c].len[k] - !!f[b].len[k];
        if (c != b && m > 0) {
          L[b].size = f[c].size;
          memcpy(L[b].at, f[c].at, sizeof L->at);
          memcpy(L[b].len, f[c].len, sizeof L->len);
        }
      }
    for (int b = 0; b < NB; b++)
      at[b] = n, n += f[b].nr < 48 ? plan(&f[b], &L[b], q + n) : 0;
    at[NB] = n;
    if (n && round == 12) e = "tile-part not found";
    if (n && !e) fetch(x, q, n);
    for (int b = 0; b < NB; b++) {
      int from = f[b].nr;
      for (int k = at[b]; k < at[b + 1]; k++)
        if (q[k].ok && !e) f[b].r[f[b].nr++] = q[k];
        else e = e ? e : q[k].e, free(q[k].b.p);
      if (!f[b].tlm) scan(&f[b], from);
    }
    if (e) break;
  }
  for (int b = 0; b < NB && !e && (bands || o->bad <= CLEAR); b++) {
    band *g = &f[b];   // the decodes
    char need[256];
    const unsigned char *pk[4096];
    uint32_t po[4096], pn[4096];
    size_t len;
    tiles(g, need);
    if (g->j.mp > 4096) e = "too many packets";
    for (int k = 0, t; k < g->j.nt && !e; k++) {
      long long h;
      const unsigned char *p = need[k] ? from(g, g->at[k], &h) : NULL;
      if (!need[k]) continue;
      if (!p || !g->len[k]) e = "tile-part not read";
      else if (!(e = j2k_part(&g->j, p, MIN(h, g->len[k]), &t, &len, po,
                              pn))) {
        for (int i = 0; i < g->j.mp; i++)
          pk[i] = po[i] + pn[i] <= h ? p + po[i] : NULL;
        e = j2k_tile(&g->j, k, pk, pn, g->x0, g->y0, g->x0 + S, g->y0 + S, 1,
                     o->v[b], S, 0);
      }
    }
    share(g, NULL, 1);
    if (b == 1 && mask) {   // no data
      for (int i = 0; i < S * S; i++) bad += !cl[i] && !o->v[1][i];
      o->bad = (float)bad / (S * S);
    }
    for (int i = 0; i < S * S; i++)
      o->v[b][i] = o->v[b][i] > off ? o->v[b][i] - off : 0;
  }
  for (int b = 0; b < NB; b++) {
    for (int i = 0; i < f[b].nr; i++) free(f[b].r[i].b.p);
    if (!f[b].nr) free(q[b].b.p);
    free(f[b].u);
  }
  return free(f), free(cl), free(L), e;
}

static int cmp(const void *a, const void *b) {
  const double *p = a, *q = b;
  int i = p[0] == q[0];
  return (p[i] > q[i]) - (p[i] < q[i]);
}

// the hull of points p[0, n) (sorted here) into h, as GEOS gives it:
// clockwise from the lowest (then leftmost) point, collinear points
// dropped, closed; its vertex count
static int hull(double (*p)[2], int n, double (*h)[2]) {
  int k = 0, s = 0;
  qsort(p, n, sizeof *p, cmp);
#define X(o, a, b) \
  ((a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]))
  for (int i = 0; i < n; i++) {   // monotone chain, right turns only
    while (k >= 2 && X(h[k - 2], h[k - 1], p[i]) >= 0) k--;
    memcpy(h[k++], p[i], sizeof *p);
  }
  for (int i = n - 2, t = k + 1; i >= 0; i--) {
    while (k >= t && X(h[k - 2], h[k - 1], p[i]) >= 0) k--;
    memcpy(h[k++], p[i], sizeof *p);
  }
  for (int i = 1, z = --k; i < z; i++)   // the last was the first
    if (h[i][1] < h[s][1] || (h[i][1] == h[s][1] && h[i][0] < h[s][0])) s = i;
  double (*r)[2] = malloc(sizeof *r * (k + 1));
  for (int i = 0; i <= k; i++) memcpy(r[i], h[(s + i) % k], sizeof *r);
  memcpy(h, r, sizeof *r * (k + 1)), free(r);
  return k;
}

// shapely 2.0's minimum rotated rectangle under GEOS 3.11: the least area
// of the hull's bounds along each edge, the first if equal; its longer side
static double axis(double (*h)[2], int k) {
  double best = INFINITY, side = 0;
  for (int i = 0; i < k; i++) {
    double dx = h[i + 1][0] - h[i][0], dy = h[i + 1][1] - h[i][1];
    double l = sqrt(dx * dx + dy * dy), ux = dx / l, uy = dy / l;
    double a0 = INFINITY, a1 = -INFINITY, b0 = INFINITY, b1 = -INFINITY;
    for (int j = 0; j <= k; j++) {
      double a = ux * h[j][0] + uy * h[j][1], b = -uy * h[j][0] + ux * h[j][1];
      a0 = MIN(a0, a), a1 = MAX(a1, a), b0 = MIN(b0, b), b1 = MAX(b1, b);
    }
    if ((a1 - a0) * (b1 - b0) < best)
      best = (a1 - a0) * (b1 - b0), side = MAX(a1 - a0, b1 - b0);
  }
  return side;
}

// np.interp(x, xp, fp)
static double interp(const net *m, double x) {
  int lo = 0, hi = m->nl - 1;
  if (x <= m->xp[0]) return m->fp[0];
  if (x >= m->xp[hi]) return m->fp[hi];
  while (hi - lo > 1) {
    int c = (lo + hi) / 2;
    if (m->xp[c] <= x) lo = c;
    else hi = c;
  }
  return x == m->xp[lo] ? m->fp[lo]
       : (m->fp[hi] - m->fp[lo]) / (m->xp[hi] - m->xp[lo]) * (x - m->xp[lo]) +
         m->fp[lo];
}

// the notebook's plumes in the model's output y (logit, conditional) over
// the box at X, Y, as json into o: the markers (peak_local_max at distance
// 1, 0.3044, borders excluded) flood (watershed) the 4-connected likelihood
// over 0.1522; each 8-connected region of that is a plume
static void plumes(const net *m, const float *y, double X, double Y,
                   FILE *o) {
  static const int D[8][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}, {1, 1},
                              {1, -1}, {-1, 1}, {-1, -1}};
  float *p = malloc(4 * S * S), mx = 0;
  int *st = malloc(4 * S * S), *lab = calloc(S * S, 4), flat = 1, np = 0;
  uint8_t *in = calloc(S * S, 1);
  for (int i = 0; i < S * S; i++)
    p[i] = 1 / (1 + expf(-y[i])), mx = MAX(mx, p[i]), flat &= p[i] == p[0];
  for (int r = 1; r < S - 1; r++)
    for (int c = 1; c < S - 1; c++) {
      int i = r * S + c, peak = !flat && p[i] > MARK;
      for (int d = 0; d < 8 && peak; d++)
        peak = p[i] >= p[i + D[d][0] * S + D[d][1]];
      if (peak && !in[i]) {   // flood, 4-connected, over the floor
        int n = 0;
        st[n++] = i, in[i] = 1;
        while (n) {
          int j = st[--n], a = j / S, b = j % S;
          for (int d = 0; d < 4; d++) {
            int u = a + D[d][0], v = b + D[d][1], w = u * S + v;
            if (u >= 0 && u < S && v >= 0 && v < S && !in[w] &&
                p[w] > FLOOR && p[w] > 0)
              in[w] = 1, st[n++] = w;
          }
        }
      }
    }
  fprintf(o, ",\"max\":%.9g,\"plumes\":[", mx);
  for (int i = 0; i < S * S; i++) {
    if (!in[i] || lab[i]) continue;
    int n = 0, z = 0, r0 = S, c0 = S, r1 = 0, c1 = 0, src = -1;
    float pm = 0;
    st[z++] = i, lab[i] = ++np;
    while (n < z) {   // the region, 8-connected
      int j = st[n++], a = j / S, b = j % S;
      r0 = MIN(r0, a), r1 = MAX(r1, a), c0 = MIN(c0, b), c1 = MAX(c1, b);
      pm = MAX(pm, p[j]);
      for (int d = 0; d < 8; d++) {
        int u = a + D[d][0], v = b + D[d][1], w = u * S + v;
        if (u >= 0 && u < S && v >= 0 && v < S && in[w] && !lab[w])
          lab[w] = np, st[z++] = w;
      }
    }
    int nc = 0;
    double ime = 0, best = 0, (*c)[2] = malloc(sizeof *c * 4 * (r1 - r0 + 1)),
           (*h)[2] = malloc(sizeof *h * (4 * (r1 - r0 + 1) + 1));
    for (int a = r0; a <= r1; a++)   // the source, its retrieval's largest
      for (int b = c0; b <= c1; b++) {
        int j = a * S + b, w = lab[j] == np;
        double v = w ? interp(m, -(double)y[S * S + j]) * p[j] / pm : 0;
        if (src < 0 || v > best) best = v, src = j;
        ime += w ? v : 0;
      }
    for (int a = r0; a <= r1; a++) {   // each row's outer pixel corners
      int l = S, r = -1;
      for (int b = c0; b <= c1; b++)
        if (lab[a * S + b] == np) l = MIN(l, b), r = MAX(r, b);
      for (int q = 0; q < 4 && r >= 0; q++)
        c[nc][0] = (q & 1 ? r + 1 : l) - c0, c[nc++][1] = a - r0 + (q >> 1);
    }
    double len = 20 * axis(h, hull(c, nc, h));
    ime *= 400;
    fprintf(o, "%s{\"x\":%.1f,\"y\":%.1f,\"pixels\":%d,\"likelihood\":%.9g,"
            "\"ime\":%.10g,\"length\":%.10g,\"q\":%.10g}", np > 1 ? "," : "",
            X + (src % S + .5) * 20, Y - (src / S + .5) * 20, z, pm, ime, len,
            1 / len * ime * 57.75286);
    free(c), free(h);
  }
  fputs("]", o);
  free(p), free(st), free(lab), free(in);
}

// jobs: a key's lines are a run of rows
typedef struct { char *key, *url; double x, y; } row;
static row *rows;
static int nrow, *runs, nrun, next;
static net M;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

// a row's read, on a thread of its own while the row before it runs the
// model, with connections of its own
typedef struct {
  ctx x;
  const row *r;
  int bands;
  crop *c;
  const char *e;
  long n, b;
  pthread_t t;
} rj;
static pthread_attr_t big;

static void *read1(void *p) {
  rj *j = p;
  long n = j->x.reads, b = j->x.bytes;
  j->e = scene(&j->x, j->r->url, j->r->x, j->r->y, 1, j->bands, j->c);
  j->n = j->x.reads - n, j->b = j->x.bytes - b;
  return NULL;
}

static void *worker(void *_) {
  rj q[2];
  crop *all[4], *fr[4], *h[2] = {NULL, NULL};
  float *in = malloc(4 * 3 * NB * S * S);
  for (int i = 0; i < 4; i++) all[i] = malloc(sizeof **all);
  open_(&q[0].x), open_(&q[1].x);
  for (;;) {
    pthread_mutex_lock(&mu);
    int k = next++, nh = 0, nf = 4;
    pthread_mutex_unlock(&mu);
    if (k >= nrun) break;
    memcpy(fr, all, sizeof fr);
    q[0].r = rows + runs[k], q[0].bands = 0, q[0].c = fr[--nf];
    pthread_create(&q[0].t, &big, read1, &q[0]);
    for (int i = runs[k]; i < runs[k + 1]; i++) {   // j: the row at hand
      rj *j = &q[(i - runs[k]) & 1], *z = &q[(i - runs[k] + 1) & 1];
      crop *c = j->c;
      pthread_join(j->t, NULL);
      int clear = !j->e && c->bad <= CLEAR, mh = nh + (clear && nh < 2);
      if (i + 1 < runs[k + 1])
        z->r = rows + i + 1, z->bands = mh == 2, z->c = fr[--nf],
        pthread_create(&z->t, &big, read1, z);
      char *s;
      size_t n;
      FILE *o = open_memstream(&s, &n);
      fprintf(o, "{\"key\":\"%s\",\"scene\":\"%s\"", j->r->key, j->r->url);
      if (j->e) fprintf(o, ",\"error\":\"%s\"", j->e);
      else fprintf(o, ",\"bad\":%.6g", c->bad);
      fprintf(o, ",\"reads\":%ld,\"mb\":%.1f", j->n, j->b / 1e6);
      if (!j->e && nh == 2 && c->bad < 1) {   // ref1, target, ref2
        const crop *a[3] = {h[0], c, h[1]};
        for (int b = 0; b < 3 * NB; b++)
          for (int p = 0; p < S * S; p++)
            in[(size_t)b * S * S + p] = a[b / NB]->v[b % NB][p] * (1. / 10000);
        ten y = forward(&M, (ten){in, 3 * NB, S, S});
        fprintf(o, ",\"refs\":[\"%s\",\"%s\"]", rows[h[0]->row].url,
                rows[h[1]->row].url);
        plumes(&M, y.p, c->X, c->Y, o), free(y.p);
      }
      fputs("}\n", o), fclose(o);
      pthread_mutex_lock(&mu);
      fputs(s, stdout), fflush(stdout);
      pthread_mutex_unlock(&mu);
      free(s);
      if (!clear) fr[nf++] = c;   // else a reference for what follows
      else {
        if (nh == 2) fr[nf++] = h[1];
        c->row = i, h[1] = h[0], h[0] = c;
      }
      nh = mh;
    }
  }
  for (int i = 0; i < 4; i++) free(all[i]);
  return free(in), NULL;
}

int main(int argc, char **argv) {
  const char *e = argc == 2 || argc == 5 ? load(&M, argv[1]) : "usage";
  if (e)
    return fprintf(stderr, "gum: %s\n%s", e, !strcmp(e, "usage") ?
                   "  gum MODEL < jobs.tsv > out.jsonl\n"
                   "  gum MODEL B12 X Y > crop.u16\n" : ""), 1;
  curl_global_init(CURL_GLOBAL_DEFAULT);
  if (argc == 5) {   // one scene's crop, as the model takes it
    ctx x;
    crop *c = malloc(sizeof *c);
    open_(&x);
    if ((e = scene(&x, argv[2], atof(argv[3]), atof(argv[4]), 0, 1, c)))
      return fprintf(stderr, "gum: %s\n", e), 1;
    return fwrite(c->v, sizeof c->v, 1, stdout) != 1;
  }
  char *l = NULL, *k = NULL;
  size_t cap = 0, rc = 0;
  runs = malloc(sizeof *runs);
  while (getline(&l, &cap, stdin) > 0) {
    char *f[4], *p = l;
    for (int i = 0; i < 4; i++) f[i] = strsep(&p, "\t\n");
    if (!f[3] || !*f[3]) continue;
    if (nrow == (int)rc)
      rows = realloc(rows, (rc = 2 * rc + 64) * sizeof *rows);
    rows[nrow] = (row){strdup(f[0]), strdup(f[3]), atof(f[1]), atof(f[2])};
    if (!k || strcmp(k, f[0]))
      runs = realloc(runs, (nrun + 2) * sizeof *runs), runs[nrun++] = nrow;
    k = rows[nrow++].key;
  }
  runs[nrun] = nrow;
  const char *nt = getenv("GUM_THREADS");
  int n = nt ? atoi(nt) : (int)sysconf(_SC_NPROCESSORS_ONLN);
  pthread_t *t = malloc(n * sizeof *t);
  pthread_attr_init(&big), pthread_attr_setstacksize(&big, 8 << 20);
  for (int i = 0; i < n; i++) pthread_create(&t[i], &big, worker, NULL);
  for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
  return 0;
}

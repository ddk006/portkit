/*
 * Looper, M7 side: records a track's output into its own file on the SD card and loops it (src/loop_shared.h).
 * bluebox eurorack 1.5.1 (re/bluebox.md).
 *
 * Where: the channel chain is player 0x0806BFDC -> EQ 0x0806AD18 -> node 0x0806BF74 -> ... -> mixer 0x0806B988
 * (fader, pan, sends). loop_process replaces the process of node 0x0806BF74 (vtable slot 3, 0x080574EC(obj, ctx)):
 * the track's bus there holds its input and clips after the EQ. Per block it
 *   - takes the track's channel parameters BARS (0x104), FILTER (0x105), FADE (0x107) from the track's queue
 *     (event type 0x39, target +8 = the node's queue id +0x18 = the track, id +0x0C, value +0x10);
 *   - writes the bus to the slot's file while recording (before the loop is added, so a new recording of a track
 *     never contains its own old loop);
 *   - adds the slot's loop (filter, fade) to the bus;
 *   - then runs the stock process.
 * Bus: 0x08053CD4(ctx, port +0x1E) -> bus; frames 0x0804D4C0(bus); stereo buffers 0x0804D598(bus, &l, &r) (as the
 * stock recorder and voices: clears a bus marked empty, makes a mono bus stereo).
 * Transport: *ctx: +0x18 float tempo, +0x30 int64 position in ticks (960 PPQ, 4/4: 3840 ticks a bar).
 * Stream cache (object 0x10044270, vtable 0x08076BD4): [0] write(cache, asset, l, r, n) 0x0805AD08 writes at the
 * asset's write position; [2] read(cache, -, int64 pos, asset, l, r, n) 0x0805B350, != 0 = ok; [5] length(cache,
 * asset) 0x0805AC88 as length(int64 *out, cache, asset) (the result goes through the first argument: called as
 * length(cache, asset) it wrote over the cache object's vtable pointer), -1 = unknown; asset map u16 at
 * +0x2708 + 2 asset, 0xFFFF = no file. The M4 opens
 * and closes the files (requests in the shared state).
 */
#include <stdint.h>
#include "loop_shared.h"

#define FN(a) ((a) | 1u)
typedef int (*process_fn)(void *obj, void *ctx);
#define fw_stock_proc ((process_fn)FN(0x080574ec))
#define fw_queue  ((void *(*)(void *, unsigned))FN(0x08053c8c))
#define fw_next_ev ((int (*)(void *, unsigned, void *))FN(0x0804e938))
#define fw_bus    ((void *(*)(void *, unsigned))FN(0x08053cd4))
#define fw_frames ((unsigned (*)(void *))FN(0x0804d4c0))
#define fw_stereo ((void (*)(void *, float **, float **))FN(0x0804d598))
#define fw_cwrite ((int (*)(void *, uint32_t, float *, float *, int))FN(0x0805ad08))
#define fw_clen   ((void (*)(int64_t *, void *, uint32_t))FN(0x0805ac88))   /* length(&out, cache, asset) */
#define CACHE ((void *)0x10044270u)
#define CACHE_MAP(a) (*(volatile uint16_t *)((uint8_t *)CACHE + 0x2708 + 2 * (a)))
#define fw_creq   ((void (*)(void *, uint32_t, uint32_t))FN(0x0805b210))
#define BLOCK 0x9800              /* samples per cache block (per channel) */

#define P_BARS 0x104
#define P_FILTER 0x105            /* -1000..1000: < 0 low-pass, > 0 high-pass, 0 off */
#define P_FADE 0x107              /* bars the loop takes to fade out, 0 = a short ramp */
#define P_QUEUE 0x18              /* the node's queue id = its channel */
#define P_PORT 0x1e
#define T_BPM 0x18
#define T_POS 0x30
#define BAR_TICKS 3840
#define MAX_BARS 16
#define XF 256                    /* crossfade at the loop point, samples (5.3 ms) */
#define MAXN 64                   /* largest block handled (the engine runs 32) */

struct fw_ev { uint8_t type, _p0[7]; uint32_t dest; uint16_t id, _p1; int32_t value; uint32_t _p2; };

/* Work buffers in SRAM4, not on the stack: loop_process runs for every track in the audio interrupt, and a frame of
 * ~1 KB there overran the stack in the simulation (it corrupted a stock analysis object, whose FFT then read from the
 * Ethernet registers). The cave has no RAM of its own (code only). */
struct work { float ll[MAXN], rr[MAXN], tl[MAXN], tr[MAXN]; };
#define WORK ((struct work *)0x38008000u)
_Static_assert(sizeof(struct work) <= 0x2000, "SRAM4 0x38008000..0x3800A000 (the M4 UI starts at 0x3800A000)");

static struct loop_shared *state(void)
{
    struct loop_shared *s = (struct loop_shared *)LOOP_SHARED;
    if (s->magic != LOOP_MAGIC) {
        for (unsigned i = 0; i < LOOP_TRACKS; i++) {
            s->cmd[i] = 0;
            s->chbars[i] = 4;
            s->chfilter[i] = 0;
            s->chfade[i] = 4;
        }
        for (unsigned i = 0; i < LOOP_SLOTS; i++) {
            struct loop_slot *sl = &s->slot[i];
            sl->state = LS_IDLE;
            sl->req = REQ_NONE;
            sl->gen = 0;
            sl->reads_failed = 0;
        }
        s->blocks = 0;
        s->magic = LOOP_MAGIC;
    }
    return s;
}

/* ---- DSP helpers ---- */
static float exp2_(float x)                          /* 2^x, |x| < 20, rel. error ~1e-4 */
{
    int i = (int)(x < 0.f ? x - 1.f : x);
    float f = x - (float)i, p = 1.f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f)));
    union { float f; int32_t i; } u = { p };
    u.i += i << 23;
    return u.f;
}

static float tan_(float x)                           /* 0 <= x < 1.4 */
{
    float x2 = x * x;
    float sn = x * (1.f - x2 * (1.f / 6 - x2 * (1.f / 120 - x2 * (1.f / 5040 - x2 * (1.f / 362880)))));
    float cs = 1.f - x2 * (0.5f - x2 * (1.f / 24 - x2 * (1.f / 720 - x2 * (1.f / 40320))));
    return sn / cs;
}

static float qsin(float x)                           /* sin(x pi / 2) on [0, 1], error ~1e-4 */
{
    float x2 = x * x;
    return x * (1.5707963f - x2 * (0.6459640f - x2 * 0.0796926f));
}

static float qcos(float x)
{
    return qsin(1.f - x);
}

/* FILTER value (-1000..1000) -> mode and coefficients: low-pass 20 kHz .. 60 Hz, high-pass 20 Hz .. 8 kHz (TPT SVF) */
static int filter_coef(float v, float *g, float *k)
{
    float fc;
    int mode;
    if (v < -5.f) {
        mode = 1;
        fc = 20000.f * exp2_(-8.380822f * (-v / 1000.f));          /* log2(20000 / 60) */
    } else if (v > 5.f) {
        mode = 2;
        fc = 20.f * exp2_(8.643856f * (v / 1000.f));               /* log2(8000 / 20) */
    } else
        return 0;
    float x = 3.14159265f * fc / 48000.f;
    *g = tan_(x > 1.35f ? 1.35f : x);
    *k = 1.25f;                                                    /* Q 0.8 */
    return mode;
}

static void svf(float *x, float *ic, int n, int mode, float g, float k)
{
    float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    float i1 = ic[0], i2 = ic[1];
    for (int j = 0; j < n; j++) {
        float v3 = x[j] - i2, v1 = a1 * i1 + a2 * v3, v2 = i2 + a2 * i1 + a3 * v3;
        i1 = 2.f * v1 - i1;
        i2 = 2.f * v2 - i2;
        x[j] = mode == 1 ? v2 : x[j] - k * v1 - v2;
    }
    ic[0] = i1;
    ic[1] = i2;
}

/* samples of `bars` bars at `bpm`, as 48.16 (no 64-bit division / conversion helpers in the cave) */
static uint64_t bars_q(unsigned bars, float bpm)
{
    double q = (double)bars * 4.0 * 60.0 * 48000.0 / (double)bpm * 65536.0 + 0.5;
    uint32_t hi = (uint32_t)(q / 4294967296.0);
    uint32_t lo = (uint32_t)(q - (double)hi * 4294967296.0);
    return ((uint64_t)hi << 32) | lo;
}

static int bar_index(const uint8_t *t)
{
    int64_t ticks = *(const int64_t *)(t + T_POS);
    return (int)((int32_t)ticks / BAR_TICKS);
}

/* ---- slots ---- */
static int slot_of(const struct loop_shared *s, unsigned track)
{
    for (int i = 0; i < LOOP_SLOTS; i++)
        if (s->slot[i].state != LS_IDLE && s->slot[i].track == track && s->slot[i].state != LS_FADE)
            return i;
    return -1;
}

static void start_fade(struct loop_shared *s, struct loop_slot *sl, const uint8_t *t)
{
    float bpm = t ? *(const float *)(t + T_BPM) : 120.f;
    if (bpm < 1.f)
        bpm = 120.f;
    unsigned bars = s->chfade[sl->track];
    sl->fade_total = bars ? (int32_t)((float)bars * 4.f * 60.f * 48000.f / bpm) : XF;
    sl->fade_left = sl->fade_total;
    sl->state = LS_FADE;
}

/* the LOOP key of `track`: arm a free slot, or stop the track's loop */
static void key(struct loop_shared *s, unsigned track, const uint8_t *t)
{
    int i = slot_of(s, track);
    if (i >= 0) {
        struct loop_slot *sl = &s->slot[i];
        if (sl->state == LS_PLAY)
            start_fade(s, sl, t);
        else {
            if (sl->state != LS_CLOSING)
                sl->req = REQ_CANCEL;       /* armed / recording: the M4 closes the file */
            sl->state = LS_IDLE;
        }
        return;
    }
    for (i = 0; i < LOOP_SLOTS; i++)
        if (s->slot[i].state == LS_IDLE)
            break;
    if (i == LOOP_SLOTS)
        return;                             /* both slots busy */
    struct loop_slot *sl = &s->slot[i];
    sl->track = (uint8_t)track;
    sl->gen ^= 1;
    sl->asset = (uint16_t)(LOOP_ASSET0 + 2 * i + sl->gen);
    sl->bars = s->chbars[track];
    sl->written = 0;
    sl->clock = 0;
    sl->armed_bar = t ? bar_index(t) : 0;
    sl->gain = 1.f;
    sl->ic[0] = sl->ic[1] = sl->ic[2] = sl->ic[3] = 0.f;
    sl->mode0 = 0;
    sl->xf_left = 0;
    sl->req = REQ_OPEN_W;
    sl->state = LS_ARMED;
}

/* Reading the loop file from the cache ourselves. The stock read (0x0805B350) finds a block through the file's
 * block table (pointer at cache + 0x352C + 4 * file slot, u16 entries indexed by b + 0x400 for b < 32, else b & 0x3FF)
 * or a short list per file, and copies without checking the block's data pointer. Descriptors (fields as the stock
 * code addresses them, at cache + 24 * e + offset): +4 float data, +8 block, +0x10 samples in it, +0x14 asset,
 * +0x16 the right channel's descriptor, +0x18 in use, +0x19 being loaded. Here a block counts only with data, not
 * being loaded, holding the samples asked for; anything else is requested (0x0805B210, asks the M4 for 4 blocks
 * from b) and read as silence, so a block that is not there yet can never fault. The copy is ours. */
struct cdesc { uint32_t _0; float *data; uint32_t block, lru, count; uint16_t asset, pair; uint8_t state, loading; };
#define CDESC(e) ((volatile struct cdesc *)((uint8_t *)CACHE + 24 * (e)))
#define NO_DATA ((float *)0xffffffffu)

/* the descriptor of block b of the asset with at least `need` samples, or -1 (then requested when stale / missing) */
static int find(uint32_t asset, uint32_t b, uint32_t need)
{
    uint16_t f = CACHE_MAP(asset);
    if (f == 0xffff)
        return -1;
    volatile uint16_t *tbl = *(volatile uint16_t *const *)((uint8_t *)CACHE + 0x352c + 4 * f);
    uint32_t i = b < 32 ? b + 0x400 : (b & 0x3ff);
    uint16_t e = tbl[i];
    if (e < 288 && CDESC(e)->asset == asset && CDESC(e)->block == b) {
        volatile struct cdesc *d = CDESC(e);
        if (d->loading)
            return -1;                  /* on its way */
        if (d->data != NO_DATA && d->count >= need)
            return e;

    }
    fw_creq(CACHE, asset, b);
    return -1;
}

/* read k samples of the asset at p into l / r; 0 (silence) when a block is not there */
static int cread(uint32_t asset, uint32_t p, float *l, float *r, int k)
{
    while (k > 0) {
        uint32_t b = p / BLOCK, off = p - b * BLOCK, m = BLOCK - off < (uint32_t)k ? BLOCK - off : (uint32_t)k;
        int e = find(asset, b, off + m);
        if (e < 0)
            return 0;
        const float *dl = CDESC(e)->data, *dr = dl;
        uint16_t pr = CDESC(e)->pair;
        if (pr < 288 && CDESC(pr)->data != NO_DATA && CDESC(pr)->count >= off + m)
            dr = CDESC(pr)->data;
        for (uint32_t j = 0; j < m; j++) {
            l[j] = dl[off + j];
            r[j] = dr[off + j];
        }
        p += m;
        l += m;
        r += m;
        k -= (int)m;
    }
    return 1;
}

/* read k samples of the loop at whole-sample position p (silence where a block is not there) */
static void get(struct loop_slot *sl, int64_t p, float *l, float *r, int k)
{
    if (!cread(sl->asset, (uint32_t)(p + sl->start), l, r, k)) {
        for (int j = 0; j < k; j++)
            l[j] = r[j] = 0.f;
        sl->reads_failed++;
    }
}

/* ask for block b ahead of reading it (find requests it unless it is loaded) */
static void prefetch(struct loop_slot *sl, uint32_t b)
{
    find(sl->asset, b, 1);
}

/* the next n samples of the loop, crossfaded at the wrap: after a jump back by `jump` samples, the first XF samples
 * mix the head x(p) fading in with the continuation x(p + jump) fading out (equal power; the file holds XF samples
 * past the loop end) */
static void __attribute__((noinline)) render(struct loop_slot *sl, float *l, float *r, int n)
{
    float *tl = WORK->tl, *tr = WORK->tr;
    int j = 0;
    /* keep the next block and, in the last quarter of a pass, the loop start in the cache */
    uint32_t st = (uint32_t)sl->start, p0 = st + (uint32_t)(sl->posq >> 16), len = (uint32_t)(sl->lenq >> 16);
    uint32_t last = ((uint32_t)sl->target - 1) / BLOCK;
    if (p0 / BLOCK + 1 <= last)
        prefetch(sl, p0 / BLOCK + 1);
    if (p0 - st > len - len / 4) {
        prefetch(sl, st / BLOCK);
        if (st / BLOCK + 1 <= last)
            prefetch(sl, st / BLOCK + 1);
    }
    while (j < n) {
        int64_t p = (int64_t)(sl->posq >> 16);
        uint64_t to_end = sl->lenq - sl->posq;
        int64_t w = (int64_t)((to_end + 0xffff) >> 16);
        int k = n - j < w ? n - j : (int)w;
        if (k < 1)
            k = 1;
        get(sl, p, l + j, r + j, k);
        if (sl->xf_left > 0) {
            int m = k < sl->xf_left ? k : sl->xf_left, done = XF - sl->xf_left;
            get(sl, p + sl->jump, tl, tr, m);
            for (int i = 0; i < m; i++) {
                float g = ((float)(done + i) + 0.5f) * (1.f / XF), a = qsin(g), b = qsin(1.f - g);
                l[j + i] = l[j + i] * a + tl[i] * b;
                r[j + i] = r[j + i] * a + tr[i] * b;
            }
            sl->xf_left -= m;
        }
        sl->posq += (uint64_t)k << 16;
        j += k;
        if (sl->posq >= sl->lenq) {
            int64_t before = (int64_t)(sl->posq >> 16);
            sl->posq -= sl->lenq;
            sl->jump = (int32_t)(before - (int64_t)(sl->posq >> 16));
            sl->xf_left = XF;
        }
    }
}

/* filter and gain ramp of a playing / fading slot for this block */
static void fx(struct loop_shared *s, struct loop_slot *sl, float *l, float *r, int n)
{
    float filt = (float)s->chfilter[sl->track], gain = 1.f;
    if (sl->state == LS_FADE) {
        sl->fade_left -= n;
        float u = sl->fade_left <= 0 ? 1.f : 1.f - (float)sl->fade_left / (float)sl->fade_total;
        gain = qcos(u);
        if (sl->fade_total > XF)
            filt += (-1000.f - filt) * u;    /* the filter closes towards low-pass 60 Hz */
    }
    float g, k;
    int mode = filter_coef(filt, &g, &k);
    if (mode != sl->mode0) {
        sl->ic[0] = sl->ic[1] = sl->ic[2] = sl->ic[3] = 0.f;
        sl->mode0 = mode;
    }
    if (mode) {
        svf(l, sl->ic, n, mode, g, k);
        svf(r, sl->ic + 2, n, mode, g, k);
    }
    float g0 = sl->gain, dg = (gain - g0) / (float)n;
    for (int j = 0; j < n; j++) {
        float gj = g0 + dg * (float)(j + 1);
        l[j] *= gj;
        r[j] *= gj;
    }
    sl->gain = gain;
    if (sl->state == LS_FADE && sl->fade_left <= 0)
        sl->state = LS_IDLE;
}

static void run_slot(struct loop_slot *sl, const uint8_t *t, float *l, float *r, int n)
{
    switch (sl->state) {
    case LS_ARMED: {
        if (!t)
            return;
        int bar = bar_index(t);
        if (bar == sl->armed_bar)
            return;
        sl->armed_bar = bar;
        if (sl->req != REQ_NONE || CACHE_MAP(sl->asset) == 0xffff)
            return;                          /* file not open yet: the next bar line */
        float bpm = *(const float *)(t + T_BPM);
        unsigned bars = sl->bars < 1 ? 1 : sl->bars > MAX_BARS ? MAX_BARS : sl->bars;
        sl->lenq = bars_q(bars, bpm < 1.f ? 120.f : bpm);
        sl->start = 0;
        sl->target = (int32_t)(sl->lenq >> 16) + XF + 64;
        sl->clock = 0;
        sl->state = LS_REC;
    }
        /* this block is the first one recorded */
        /* fall through */
    case LS_REC:
        fw_cwrite(CACHE, sl->asset, l, r, n);
        sl->written += n;
        sl->clock += (uint32_t)n;
        if (sl->written >= sl->target) {
            sl->req = REQ_CLOSE;
            sl->state = LS_CLOSING;
        }
        return;
    case LS_CLOSING: {
        int64_t flen = -1;
        if (sl->req == REQ_NONE)
            fw_clen(&flen, CACHE, sl->asset);
        if (sl->req != REQ_NONE || flen < (int64_t)sl->target) {
            sl->clock += (uint32_t)n;
            return;
        }
        /* the loop joins at the phase the transport has reached since the recording started (this block starts
         * `clock` samples after the loop start), once the block there is loaded (asked for meanwhile) */
        uint64_t q = (uint64_t)sl->clock << 16;
        while (q >= sl->lenq)
            q -= sl->lenq;
        uint32_t p = (uint32_t)sl->start + (uint32_t)(q >> 16);
        uint32_t need = p % BLOCK + (uint32_t)n;
        if (find(sl->asset, p / BLOCK, need < BLOCK ? need : BLOCK) < 0) {
            sl->clock += (uint32_t)n;
            return;
        }
        sl->posq = q;
        sl->xf_left = 0;
        sl->gain = 0.f;                      /* ramp in over the first block */
        sl->state = LS_PLAY;
        return;
    }
    default:
        return;
    }
}

int loop_process(void *obj, void *ctx)
{
    struct loop_shared *s = state();
    unsigned track = *(uint32_t *)((uint8_t *)obj + P_QUEUE);
    if (track >= LOOP_TRACKS || !ctx)
        return fw_stock_proc(obj, ctx);
    const uint8_t *t = *(const uint8_t **)ctx;
    if (track == 0)
        s->blocks++;
    void *q = fw_queue(ctx, track);
    struct fw_ev ev;
    for (unsigned i = 0; fw_next_ev(q, i, &ev); i++) {
        if (ev.type != 0x39 || ev.dest != track)
            continue;
        if (ev.id == P_BARS)
            s->chbars[track] = (uint8_t)(ev.value < 1 ? 1 : ev.value > MAX_BARS ? MAX_BARS : ev.value);
        else if (ev.id == P_FILTER)
            s->chfilter[track] = (int16_t)(ev.value < -1000 ? -1000 : ev.value > 1000 ? 1000 : ev.value);
        else if (ev.id == P_FADE)
            s->chfade[track] = (uint8_t)(ev.value < 0 ? 0 : ev.value > MAX_BARS ? MAX_BARS : ev.value);
    }
    if (s->cmd[track]) {
        s->cmd[track] = 0;
        key(s, track, t);
    }
    int busy = 0;
    for (int i = 0; i < LOOP_SLOTS; i++)
        if (s->slot[i].state != LS_IDLE && s->slot[i].track == track)
            busy = 1;
    if (busy) {
        void *bus = fw_bus(ctx, *(uint16_t *)((uint8_t *)obj + P_PORT));
        int n = (int)fw_frames(bus);
        float *l = 0, *r = 0;
        fw_stereo(bus, &l, &r);
        if (l && r && n > 0 && n <= MAXN) {
            for (int i = 0; i < LOOP_SLOTS; i++) {      /* record first: the bus without this track's loops */
                struct loop_slot *sl = &s->slot[i];
                if (sl->track == track && sl->state >= LS_ARMED && sl->state <= LS_CLOSING)
                    run_slot(sl, t, l, r, n);
            }
            for (int i = 0; i < LOOP_SLOTS; i++) {
                struct loop_slot *sl = &s->slot[i];
                if (sl->track != track || (sl->state != LS_PLAY && sl->state != LS_FADE))
                    continue;
                float *ll = WORK->ll, *rr = WORK->rr;
                render(sl, ll, rr, n);
                fx(s, sl, ll, rr, n);
                for (int j = 0; j < n; j++) {
                    l[j] += ll[j];
                    r[j] += rr[j];
                }
            }
        }
    }
    return fw_stock_proc(obj, ctx);
}

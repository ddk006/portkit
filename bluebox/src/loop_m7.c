/*
 * Looper prototype, M7 side: a clip on a track repeats instead of ending.
 * bluebox eurorack 1.5.1 (re/bluebox.md, "Track-Player").
 *
 * The track player (channel node vtable 0x0806BF74, player 0x08057B78) renders each clip as a voice through
 * FUN_08057A00(player, voice, bus, a3). Voice (0x38 bytes): +0x00 active, +0x01 ending (fading out), +0x04 asset id,
 * +0x08 int64 read position in the WAV (samples), +0x18 int64 start delay, +0x20 int64 samples left in the clip.
 * When the samples left drop below one, the stock code fades the voice out and starts the player's next clip.
 *
 * loop_voice replaces the three calls of FUN_08057A00 (0x08057D1E, 0x08057EF8, 0x08057F06). For a looping track it
 * fixes the loop region at the first block it sees of a voice:
 *   - bars = 0: the whole clip;
 *   - bars = 1..16: from the first bar line at or after the point where the clip plays on the timeline, `bars` bars
 *     long (4/4: 3840 ticks a bar, 960 PPQ), in samples at the current tempo; fewer bars if the clip is too short
 *     (whole clip if not even one fits with XF samples to spare).
 * The fourth argument a3 points to the transport pointer (as passed on to the voice start 0x0805779C): transport
 * +0x18 float tempo (BPM), +0x30 int64 position in ticks; samples = ticks / 960 / bpm * 60 * 48000 (0x0805AB80).
 * When a block would reach the loop end, the samples left get one loop length added first (so the stock code doesn't
 * end the clip); after the block the read position moves back by that length. left = clip end - pos holds throughout.
 * A bar is rarely a whole number of samples: the jump lengths come from a 48.16 fixed-point accumulator, so over many
 * passes the loop stays on the transport's bar grid (no drift). Tempo changes while looping are not followed yet.
 *
 * The jump happens at a block boundary (32 frames): the last block of a pass reads up to 31 samples past the clip end,
 * the next pass starts at the matching offset. The click there is removed by a crossfade in the cache read:
 * loop_read replaces the stream cache's read (vtable 0x08076BD4 slot 2, 0x0805B350(this, fn, int64 pos, asset,
 * float *l, float *r, count), returns != 0 on success; the voice's only call is 0x08057A92). After a jump, reads in
 * [start, start + XF) are mixed from the head x(p) fading in and the continuation past the loop end x(p + len) fading
 * out (equal power). The pass before the jump played up to that continuation, so the signal stays continuous; the
 * recording should run at least XF samples past the loop end. If the file ends at the loop end, the stock read fails
 * for the block that reaches past it; loop_read then reads the part up to the loop end and pads with silence, and the
 * head fades in from silence after the jump (a step at the loop end remains: the looper records past the end).
 *
 * Which tracks loop: a bit mask in shared state (LOOP_ALL by default), no UI yet.
 *
 * State: SRAM4 0x3800F000 (unused by the stock firmware: re/bluebox.md, canary check in the simulation).
 */
#include <stdint.h>

#define FN(a) ((a) | 1u)
typedef void (*voice_fn)(void *player, void *voice, void *bus, uint32_t a3);
typedef unsigned (*frames_fn)(void *bus);
typedef int (*read_fn)(void *cache, void *fn, int64_t pos, uint32_t asset, float *l, float *r, int n);
#define fw_voice  ((voice_fn)FN(0x08057a00))
#define fw_frames ((frames_fn)FN(0x0804d4c0))
#define fw_read   ((read_fn)FN(0x0805b350))
#define XF 256                    /* crossfade at the loop point, samples (5.3 ms at 48 kHz) */
#define MAXN 64                   /* largest read we blend (the voice reads one block of 32) */

struct voice {
    uint8_t active, ending, _p[2];
    uint32_t asset;
    int64_t pos;
    uint32_t _p1[2];
    int64_t delay;
    int64_t left;
};

#define SLOTS 32                  /* voices tracked at once (12 tracks x up to 3 voices) */
struct loop_slot {
    struct voice *v;              /* 0 = free */
    uint32_t asset;
    int64_t start, len;           /* loop region in the file; len = the next (or last) jump */
    uint32_t passes;              /* jumps done on this clip: the crossfade starts with the first */
    uint32_t xf0;                 /* where the last jump landed, samples after start: the crossfade starts there */
    int64_t cend;                 /* clip end in the file */
    int64_t lenq, acc;            /* loop length and jump accumulator, 48.16 fixed point */
};
struct loop_state {
    uint32_t magic;
    uint32_t mask;                /* bit n: track (player queue id) n loops */
    uint32_t wraps;               /* jumps done, for tests */
    uint32_t bars;                /* loop length in bars, 0 = whole clip (set before a clip starts) */
    struct loop_slot slot[SLOTS];
};
#define LS ((volatile struct loop_state *)0x3800F000u)
#define LS_MAGIC 0x504f4f4cu      /* "LOOP" */
#define LOOP_ALL 0x0fffu
#define P_QUEUE 0x18              /* the player node's queue id = its channel */
#define T_BPM 0x18                /* transport: float tempo */
#define T_POS 0x30                /* transport: int64 position, ticks */
#define BAR_TICKS 3840            /* 4/4 at 960 PPQ */
#define MAX_BARS 16

static struct loop_state *state(void)
{
    struct loop_state *s = (struct loop_state *)LS;
    if (s->magic != LS_MAGIC) {
        for (unsigned i = 0; i < SLOTS; i++)
            s->slot[i].v = 0;
        s->mask = LOOP_ALL;
        s->wraps = 0;
        s->bars = 0;
        s->magic = LS_MAGIC;
    }
    return s;
}

static struct loop_slot *find(struct loop_state *s, struct voice *v, int make)
{
    struct loop_slot *free_ = 0;
    for (unsigned i = 0; i < SLOTS; i++) {
        if (s->slot[i].v == v)
            return &s->slot[i];
        if (!s->slot[i].v && !free_)
            free_ = &s->slot[i];
    }
    if (make && free_) {
        free_->v = v;
        free_->len = 0;
        free_->passes = 0;
    }
    return make ? free_ : 0;
}

/* Fix the loop region of a voice seen for the first time (or a new clip on it). */
static void region(struct loop_state *s, struct loop_slot *sl, struct voice *v, uint32_t a3)
{
    sl->asset = v->asset;
    sl->passes = 0;
    sl->acc = 0;
    sl->cend = v->pos + v->left;
    sl->start = v->pos;                                  /* bars = 0 / fallback: the whole clip */
    sl->lenq = v->left << 16;
    unsigned bars = s->bars > MAX_BARS ? MAX_BARS : s->bars;
    uint8_t *t = a3 ? *(uint8_t **)a3 : 0;
    float bpm = t ? *(float *)(t + T_BPM) : 0.f;
    if (bars && bpm > 1.f) {
        /* 32-bit is plenty here (2^31 ticks are > 300 h at 120 BPM, 2^31 samples 12 h) and keeps the conversions on
         * the FPU (int64 <-> double would need libgcc) */
        double spt = 48000.0 * 60.0 / (960.0 * bpm);    /* samples per tick */
        int32_t pos = (int32_t)v->pos, cend = (int32_t)sl->cend;
        double t0 = (double)(int32_t)*(int64_t *)(t + T_POS) + (double)(int32_t)v->delay / spt;  /* tick of v->pos */
        int32_t bar = ((int32_t)t0 + BAR_TICKS - 1) / BAR_TICKS * BAR_TICKS;
        if ((double)bar < t0)
            bar += BAR_TICKS;
        int32_t ls = pos + (int32_t)(((double)bar - t0) * spt + 0.5);
        double barlen = BAR_TICKS * spt;
        while (bars && (double)ls + bars * barlen + XF > (double)cend)
            bars--;                                      /* fewer bars if the clip is too short */
        if (bars) {
            double q = bars * barlen * 65536.0 + 0.5;    /* < 2^47: split so each part converts in 32 bits */
            uint32_t hi = (uint32_t)(q / 4294967296.0);
            uint32_t lo = (uint32_t)(q - (double)hi * 4294967296.0);
            sl->start = ls;
            sl->lenq = ((int64_t)hi << 32) | lo;
        }
    }
    sl->len = sl->lenq >> 16;
}

void loop_voice(void *player, void *voice, void *bus, uint32_t a3)
{
    struct loop_state *s = state();
    struct voice *v = voice;
    unsigned track = *(uint32_t *)((uint8_t *)player + P_QUEUE);

    if (!v->active || v->ending || track > 31 || !(s->mask >> track & 1)) {
        struct loop_slot *sl = find(s, v, 0);
        if (sl)
            sl->v = 0;                                   /* voice ended or not looping: forget it */
        fw_voice(player, voice, bus, a3);
        return;
    }
    struct loop_slot *sl = find(s, v, 1);
    if (!sl) {                                           /* table full: stock behaviour */
        fw_voice(player, voice, bus, a3);
        return;
    }
    if (sl->len <= 0 || sl->asset != v->asset || v->pos + v->left != sl->cend)
        region(s, sl, v, a3);                            /* new clip on this voice */
    int64_t n = fw_frames(bus);
    int wrap = sl->len > n && v->pos + n >= sl->start + sl->len;
    if (wrap)
        v->left += sl->len;                              /* keep the stock code from ending the clip */
    fw_voice(player, voice, bus, a3);
    if (wrap && v->active) {
        v->pos -= sl->len;                               /* left (+len before the block) = clip end - pos again */
        sl->xf0 = (uint32_t)(v->pos - sl->start);
        sl->passes++;
        s->wraps++;
        /* next jump: whole samples from the accumulator, the fractions carried over */
        sl->acc += sl->lenq - ((int64_t)sl->len << 16);
        sl->len = (sl->lenq + sl->acc) >> 16;
    }
}

/* sin(x pi / 2) on [0, 1], error ~1e-4 */
static float qsin(float x)
{
    float x2 = x * x;
    return x * (1.5707963f - x2 * (0.6459640f - x2 * 0.0796926f));
}

static struct loop_slot *slot_of(struct loop_state *s, uint32_t asset)
{
    for (unsigned i = 0; i < SLOTS; i++)
        if (s->slot[i].v && s->slot[i].asset == asset)
            return &s->slot[i];
    return 0;
}

int loop_read(void *cache, void *fn, int64_t pos, uint32_t asset, float *l, float *r, int n)
{
    int ok = fw_read(cache, fn, pos, asset, l, r, n);
    if (n <= 0 || n > MAXN)
        return ok;
    struct loop_state *s = state();
    struct loop_slot *sl = slot_of(s, asset);
    if (!sl)
        return ok;
    int64_t end = sl->start + sl->len;
    if (!ok) {
        /* the last block of a pass reaches past the end of the file: the stock read fails for all of it and the
         * voice would play silence; read the part up to the loop end, silence after it */
        if (!(pos < end && pos + n > end) || !fw_read(cache, fn, pos, asset, l, r, (int)(end - pos)))
            return ok;
        for (int i = (int)(end - pos); i < n; i++) {
            if (l)
                l[i] = 0.f;
            if (r)
                r[i] = 0.f;
        }
        ok = 1;
    }
    if (!sl->passes)
        return ok;
    int64_t off = pos - sl->start - sl->xf0;            /* samples since the jump landed */
    if (off >= XF || off + n <= 0)
        return ok;
    float tl[MAXN], tr[MAXN];
    if (!fw_read(cache, fn, pos + sl->len, asset, l ? tl : 0, r ? tr : 0, n))
        for (int i = 0; i < n; i++)
            tl[i] = tr[i] = 0.f;                         /* past the end of the file: fade in from silence */
    for (int i = 0; i < n; i++) {
        int64_t k = off + i;
        if (k < 0 || k >= XF)
            continue;
        float g = ((float)k + 0.5f) * (1.f / XF), a = qsin(g), b = qsin(1.f - g);
        if (l)
            l[i] = l[i] * a + tl[i] * b;
        if (r)
            r[i] = r[i] * a + tr[i] * b;
    }
    return ok;
}

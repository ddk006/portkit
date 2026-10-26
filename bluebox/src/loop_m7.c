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
 * Which tracks loop: the channel parameters LOOP (0x103) and BARS (0x104, 1..16; src/loop_m4.c) arrive in each track's
 * queue. loop_process wraps the track player node's process (vtable 0x0806BF74 slot 3, 0x080574EC(obj, ctx)) and picks
 * them up (event type 0x39, target queue +8 = the node's queue id +0x18 = the track, id +0x0C, value +0x10). Two loop
 * slots: tracks get one in the order they switch LOOP on; a third track with LOOP on waits and gets the next slot
 * that frees up. LOOP off ends the looping clip with the stock fade (its samples left are cut to this block). A debug mask
 * (state +4) makes tracks loop regardless (tests); BARS from the track, else the state's global `bars`.
 *
 * State: SRAM4 0x3800F000 (unused by the stock firmware: re/bluebox.md, canary check in the simulation).
 */
#include <stdint.h>

#define FN(a) ((a) | 1u)
typedef void (*voice_fn)(void *player, void *voice, void *bus, uint32_t a3);
typedef unsigned (*frames_fn)(void *bus);
typedef int (*read_fn)(void *cache, void *fn, int64_t pos, uint32_t asset, float *l, float *r, int n);
typedef int (*process_fn)(void *obj, void *ctx);
typedef void *(*queue_fn)(void *ctx, unsigned id);
typedef int (*event_fn)(void *queue, unsigned index, void *event);
#define fw_voice  ((voice_fn)FN(0x08057a00))
#define fw_frames ((frames_fn)FN(0x0804d4c0))
#define fw_read   ((read_fn)FN(0x0805b350))
#define fw_player ((process_fn)FN(0x080574ec))
#define fw_queue  ((queue_fn)FN(0x08053c8c))
#define fw_next_ev ((event_fn)FN(0x0804e938))
#define P_LOOP 0x103
#define P_BARS 0x104
#define P_FILTER 0x105            /* -1000..1000: < 0 low-pass, > 0 high-pass, 0 off */
#define P_FADE 0x107              /* bars the loop takes to fade out when LOOP goes off, 0 = at once */
#define TRACKS 12
#define LOOP_SLOTS 2              /* loops at once; more would work the same way (SD bandwidth: not measured) */

struct fw_ev { uint8_t type, _p0[7]; uint32_t dest; uint16_t id, _p1; int32_t value; uint32_t _p2; };
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
    uint32_t bars;                /* loop length in bars when a track has none, 0 = whole clip (tests) */
    struct loop_slot slot[SLOTS];
    uint8_t owner[LOOP_SLOTS];    /* track + 1 holding each loop slot, 0 = free */
    uint8_t chbars[TRACKS];       /* BARS per track, 0 = not received */
    uint16_t want;                /* bit n: track n has LOOP on (with or without a slot) */
    int16_t chfilter[TRACKS];     /* FILTER per track */
    uint8_t chfade[TRACKS];       /* FADE per track, bars */
    struct track_fx {
        float ic[4];              /* SVF state: L ic1 ic2, R ic1 ic2 */
        float gain;               /* gain at the end of the last block */
        float g0, k0;             /* filter coefficients of the last block (g = tan(pi fc / fs), k = 1/Q) */
        int32_t mode0;            /* filter mode of the last block: 0 off, 1 low-pass, 2 high-pass */
        int32_t fade_left, fade_total;   /* samples; fade_total 0 = not fading */
        uint8_t fading;           /* LOOP went off with FADE > 0: the loop keeps its slot until the fade ends */
        uint8_t seen;             /* a looping voice of this track rendered since the last loop_process */
        uint8_t _p[2];
    } fx[TRACKS];
    /* the block being rendered (set by loop_voice around the stock voice call, read by loop_read) */
    int32_t cur_track;            /* -1 = none */
    uint32_t cur_asset;
    float cur_g0, cur_g1;         /* gain ramp across the block */
    float cur_fg, cur_fk;         /* filter coefficients for the block */
    int32_t cur_mode;
};
#include <stddef.h>
_Static_assert(sizeof(struct loop_slot) == 56 && offsetof(struct loop_state, slot) == 16, "state layout (tests, sim)");
_Static_assert(offsetof(struct loop_state, owner) == 0x710 && offsetof(struct loop_state, chbars) == 0x712 &&
               offsetof(struct loop_state, want) == 0x71e, "state layout (tests, sim)");
#define LS ((volatile struct loop_state *)0x3800F000u)
#define LS_MAGIC 0x354f4f4cu      /* "LOO5": layout version; any other value (old layout, power-up) re-initialises */
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
        s->mask = 0;
        s->wraps = 0;
        s->bars = 0;
        for (unsigned i = 0; i < LOOP_SLOTS; i++)
            s->owner[i] = 0;
        for (unsigned i = 0; i < TRACKS; i++)
            s->chbars[i] = 0;
        s->want = 0;
        for (unsigned i = 0; i < TRACKS; i++) {
            s->chfilter[i] = 0;
            s->chfade[i] = 0;
            struct track_fx *f = &s->fx[i];
            f->ic[0] = f->ic[1] = f->ic[2] = f->ic[3] = 0.f;
            f->gain = 1.f;
            f->fading = 0;
            f->fade_total = f->fade_left = 0;
            f->mode0 = 0;
        }
        s->cur_track = -1;
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
static void region(struct loop_slot *sl, struct voice *v, uint32_t a3, unsigned bars)
{
    sl->asset = v->asset;
    sl->passes = 0;
    sl->acc = 0;
    sl->cend = v->pos + v->left;
    sl->start = v->pos;                                  /* bars = 0 / fallback: the whole clip */
    sl->lenq = v->left << 16;
    if (bars > MAX_BARS)
        bars = MAX_BARS;
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

static int owns(const struct loop_state *s, unsigned track)
{
    for (unsigned i = 0; i < LOOP_SLOTS; i++)
        if (s->owner[i] == track + 1)
            return 1;
    return 0;
}

static int looping(const struct loop_state *s, unsigned track)
{
    return track < 32 && ((s->mask >> track & 1) || (track < TRACKS && owns(s, track)));
}

/* ---- filter and fade ---- */
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

/* FILTER value (-1000..1000) -> mode and coefficients: low-pass 20 kHz .. 60 Hz, high-pass 20 Hz .. 8 kHz */
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
    *k = 1.25f;                                                    /* Q 0.8: a touch of resonance */
    return mode;
}

static float qcos(float x);                          /* cos(x pi / 2) on [0, 1] (below) */

/* Gain and filter for this block of `track`'s loop (called by loop_voice before the stock voice renders it). */
static void fx_block(struct loop_state *s, unsigned track, unsigned n, uint32_t a3)
{
    struct track_fx *f = &s->fx[track];
    float filt = (float)s->chfilter[track], gain = 1.f;
    if (f->fading) {
        if (!f->fade_total) {                            /* the fade starts: its length at the current tempo */
            uint8_t *t = a3 ? *(uint8_t **)a3 : 0;
            float bpm = t ? *(float *)(t + 0x18) : 120.f;
            if (bpm < 1.f)
                bpm = 120.f;
            f->fade_total = (int32_t)((float)s->chfade[track] * 4.f * 60.f * 48000.f / bpm);
            f->fade_left = f->fade_total;
        }
        f->fade_left -= (int32_t)n;
        float u = f->fade_left <= 0 ? 1.f : 1.f - (float)f->fade_left / (float)f->fade_total;
        gain = qcos(u);
        filt += (-1000.f - filt) * u;                    /* the filter closes towards low-pass 60 Hz */
    }
    s->cur_track = (int32_t)track;
    s->cur_g0 = f->gain;
    s->cur_g1 = gain;
    f->gain = gain;
    s->cur_mode = filter_coef(filt, &s->cur_fg, &s->cur_fk);
    if (s->cur_mode != f->mode0) {                       /* mode change: start the filter from rest */
        f->ic[0] = f->ic[1] = f->ic[2] = f->ic[3] = 0.f;
        f->mode0 = s->cur_mode;
    }
}

/* apply gain ramp and filter to the block the voice just read (one channel) */
static void fx_apply(struct loop_state *s, float *x, float *ic, int n)
{
    float g0 = s->cur_g0, dg = (s->cur_g1 - s->cur_g0) / (float)n;
    if (s->cur_mode) {
        float g = s->cur_fg, k = s->cur_fk;
        float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
        float i1 = ic[0], i2 = ic[1];
        for (int j = 0; j < n; j++) {
            float v3 = x[j] - i2, v1 = a1 * i1 + a2 * v3, v2 = i2 + a2 * i1 + a3 * v3;
            i1 = 2.f * v1 - i1;
            i2 = 2.f * v2 - i2;
            x[j] = s->cur_mode == 1 ? v2 : x[j] - k * v1 - v2;
        }
        ic[0] = i1;
        ic[1] = i2;
    }
    if (g0 != 1.f || dg != 0.f)
        for (int j = 0; j < n; j++)
            x[j] *= g0 + dg * (float)(j + 1);
}

static void set_loop(struct loop_state *s, unsigned track, int on);
static void release(struct loop_state *s, unsigned track);

void loop_voice(void *player, void *voice, void *bus, uint32_t a3)
{
    struct loop_state *s = state();
    struct voice *v = voice;
    unsigned track = *(uint32_t *)((uint8_t *)player + P_QUEUE);

    s->cur_track = -1;                                   /* set again by fx_block for a looping block */
    if (!v->active || v->ending || !looping(s, track)) {
        struct loop_slot *sl = find(s, v, 0);
        if (sl) {
            sl->v = 0;                                   /* voice ended or LOOP off: forget it */
            if (v->active && !v->ending && sl->passes) {
                /* LOOP switched off while looping: end the clip at this block (the stock fade and next clip follow),
                 * instead of playing on past the loop into the rest of the recording */
                int64_t n = fw_frames(bus);
                if (v->left > n)
                    v->left = n;
            }
        }
        fw_voice(player, voice, bus, a3);
        return;
    }
    struct loop_slot *sl = find(s, v, 1);
    if (!sl) {                                           /* table full: stock behaviour */
        fw_voice(player, voice, bus, a3);
        return;
    }
    if (sl->len <= 0 || sl->asset != v->asset || v->pos + v->left != sl->cend)
        region(sl, v, a3, track < TRACKS && s->chbars[track] ? s->chbars[track] : s->bars);   /* new clip */
    int64_t n = fw_frames(bus);
    if (track < TRACKS && s->fx[track].fading && s->fx[track].fade_total && s->fx[track].fade_left <= 0) {
        /* the fade is over: give the slot back and end the clip at this block (silent by now) */
        s->fx[track].fading = 0;
        s->fx[track].fade_total = 0;
        s->fx[track].gain = 1.f;
        release(s, track);
        sl->v = 0;
        if (v->left > n)
            v->left = n;
        fw_voice(player, voice, bus, a3);
        return;
    }
    int wrap = sl->len > n && v->pos + n >= sl->start + sl->len;
    if (wrap)
        v->left += sl->len;                              /* keep the stock code from ending the clip */
    if (track < TRACKS) {
        fx_block(s, track, (unsigned)n, a3);
        s->cur_asset = v->asset;
        s->fx[track].seen = 1;
    }
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

/* LOOP on/off for a track. With FADE > 0, LOOP off keeps the slot while the loop fades out (loop_voice gives it back
 * at the end); LOOP on again during the fade cancels it. Slots go in switch-on order; a track that switched on while both were taken gets the next
 * slot that frees up (lowest track number first), as its LOOP stays on and is not sent again. */
static void set_loop(struct loop_state *s, unsigned track, int on)
{
    struct track_fx *f = &s->fx[track];
    if (on) {
        s->want |= (uint16_t)(1u << track);
        f->fading = 0;
        f->fade_total = 0;
    } else {
        s->want &= (uint16_t)~(1u << track);
        if (owns(s, track) && s->chfade[track] && !f->fading) {
            f->fading = 1;                               /* fade out first (loop_voice), keep the slot meanwhile */
            f->fade_total = 0;
            return;
        }
    }
    for (unsigned i = 0; i < LOOP_SLOTS; i++)
        if (s->owner[i] && !(s->want >> (s->owner[i] - 1) & 1) && !s->fx[s->owner[i] - 1].fading)
            s->owner[i] = 0;
    if (on && !owns(s, track))
        for (unsigned i = 0; i < LOOP_SLOTS; i++)
            if (!s->owner[i]) {
                s->owner[i] = (uint8_t)(track + 1);
                return;
            }
    for (unsigned i = 0; i < LOOP_SLOTS; i++)         /* fill freed slots from the waiting tracks */
        for (unsigned t = 0; !s->owner[i] && t < TRACKS; t++)
            if ((s->want >> t & 1) && !owns(s, t))
                s->owner[i] = (uint8_t)(t + 1);
}

/* give a track's slot back (end of its fade) and hand free slots to waiting tracks */
static void release(struct loop_state *s, unsigned track)
{
    for (unsigned i = 0; i < LOOP_SLOTS; i++)
        if (s->owner[i] == track + 1)
            s->owner[i] = 0;
    for (unsigned i = 0; i < LOOP_SLOTS; i++)
        for (unsigned t = 0; !s->owner[i] && t < TRACKS; t++)
            if ((s->want >> t & 1) && !owns(s, t))
                s->owner[i] = (uint8_t)(t + 1);
}

int loop_process(void *obj, void *ctx)
{
    struct loop_state *s = state();
    unsigned track = *(uint32_t *)((uint8_t *)obj + P_QUEUE);
    if (track < TRACKS) {
        /* a fade with nothing playing (transport stopped, no clip) would never end: free the slot at once */
        if (s->fx[track].fading && !s->fx[track].seen) {
            s->fx[track].fading = 0;
            s->fx[track].fade_total = 0;
            s->fx[track].gain = 1.f;
            release(s, track);
        }
        s->fx[track].seen = 0;
        void *q = fw_queue(ctx, track);
        struct fw_ev ev;
        for (unsigned i = 0; fw_next_ev(q, i, &ev); i++) {
            if (ev.type != 0x39 || ev.dest != track)
                continue;
            if (ev.id == P_LOOP)
                set_loop(s, track, ev.value != 0);
            else if (ev.id == P_BARS)
                s->chbars[track] = (uint8_t)(ev.value < 1 ? 1 : ev.value > MAX_BARS ? MAX_BARS : ev.value);
            else if (ev.id == P_FILTER)
                s->chfilter[track] = (int16_t)(ev.value < -1000 ? -1000 : ev.value > 1000 ? 1000 : ev.value);
            else if (ev.id == P_FADE)
                s->chfade[track] = (uint8_t)(ev.value < 0 ? 0 : ev.value > MAX_BARS ? MAX_BARS : ev.value);
        }
    }
    return fw_player(obj, ctx);
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

static float qcos(float x)
{
    return qsin(1.f - x);
}

static int loop_read_(void *cache, void *fn, int64_t pos, uint32_t asset, float *l, float *r, int n);

int loop_read(void *cache, void *fn, int64_t pos, uint32_t asset, float *l, float *r, int n)
{
    int ok = loop_read_(cache, fn, pos, asset, l, r, n);
    struct loop_state *s = state();
    if (ok && n > 0 && s->cur_track >= 0 && s->cur_track < TRACKS && s->cur_asset == asset) {
        struct track_fx *f = &s->fx[s->cur_track];
        if (l)
            fx_apply(s, l, f->ic, n);
        if (r)
            fx_apply(s, r, f->ic + 2, n);
    }
    return ok;
}

static int loop_read_(void *cache, void *fn, int64_t pos, uint32_t asset, float *l, float *r, int n)
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

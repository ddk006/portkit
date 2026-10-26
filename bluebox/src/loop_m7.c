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
 * remembers where the clip started (position and length, at the first block it sees of that voice), and when this
 * block would end the clip it adds one clip length to the samples left first; after the block it moves the read
 * position back by one length. The invariant  left = len - (pos - start)  holds across the jump, so the loop keeps
 * its length exactly (no drift).
 *
 * Prototype limits (next steps): the jump happens at a block boundary (32 frames): the last block of a pass reads up
 * to 31 samples past the clip end before the next pass starts at the matching offset (0.7 ms, a possible click; a
 * crossfade comes later). Which tracks loop: a bit mask in shared state (LOOP_ALL by default), no UI yet.
 *
 * State: SRAM4 0x3800F000 (unused by the stock firmware: re/bluebox.md, canary check in the simulation).
 */
#include <stdint.h>

#define FN(a) ((a) | 1u)
typedef void (*voice_fn)(void *player, void *voice, void *bus, uint32_t a3);
typedef unsigned (*frames_fn)(void *bus);
#define fw_voice  ((voice_fn)FN(0x08057a00))
#define fw_frames ((frames_fn)FN(0x0804d4c0))

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
    int64_t start, len;
};
struct loop_state {
    uint32_t magic;
    uint32_t mask;                /* bit n: track (player queue id) n loops */
    uint32_t wraps;               /* jumps done, for tests */
    uint32_t _p;
    struct loop_slot slot[SLOTS];
};
#define LS ((volatile struct loop_state *)0x3800F000u)
#define LS_MAGIC 0x504f4f4cu      /* "LOOP" */
#define LOOP_ALL 0x0fffu
#define P_QUEUE 0x18              /* the player node's queue id = its channel */

static struct loop_state *state(void)
{
    struct loop_state *s = (struct loop_state *)LS;
    if (s->magic != LS_MAGIC) {
        for (unsigned i = 0; i < SLOTS; i++)
            s->slot[i].v = 0;
        s->mask = LOOP_ALL;
        s->wraps = 0;
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
    }
    return make ? free_ : 0;
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
    /* a new clip on this voice (or the first block we see): remember start and length */
    if (sl->len <= 0 || sl->asset != v->asset || v->pos < sl->start || v->pos - sl->start > sl->len) {
        sl->asset = v->asset;
        sl->start = v->pos;
        sl->len = v->left;
    }
    int64_t n = fw_frames(bus);
    int wrap = sl->len > n && v->left <= n;
    if (wrap)
        v->left += sl->len;                              /* keep the stock code from ending the clip */
    fw_voice(player, voice, bus, a3);
    if (wrap && v->active) {
        v->pos -= sl->len;
        s->wraps++;
    }
}

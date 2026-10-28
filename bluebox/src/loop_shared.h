/*
 * Looper state shared by the M7 (src/loop_m7.c, audio) and the M4 (src/loop_ui_m4.c, UI and SD files).
 * SRAM4 0x3800F000 (unused by the stock firmware; both cores see it at the same address).
 *
 * A loop slot records a track's output (after its EQ) into its own WAV file on the SD card through the stock stream
 * cache, then plays the file back in a loop, mixed into the same track. It does not use the track recorder, so it
 * works while tracks or the master are being recorded.
 *
 *   IDLE --(LOOP key, M4 sets cmd)--> ARMED: M7 asks the M4 to open the slot's file for writing (req OPEN_W)
 *   ARMED --(next bar line, file open)--> REC: every block the track's output goes to the cache (write, vtable[0])
 *   REC --(BARS bars + XF samples written)--> CLOSING: M7 asks the M4 to close the file and open it for reading
 *   CLOSING --(the cache knows the file's length)--> PLAY: the file loops from the matching phase, crossfaded
 *   PLAY --(LOOP key)--> FADE (FADE bars, filter closes) --> IDLE
 *   ARMED / REC / CLOSING --(LOOP key)--> IDLE (req CANCEL: the M4 closes the file)
 *
 * Handshake: cmd[track] is written by the M4 and cleared by the M7; slot.req is written by the M7 and cleared by
 * the M4 once it has queued the stream requests. Everything else in a slot is written by the M7 only.
 */
#ifndef LOOP_SHARED_H
#define LOOP_SHARED_H
#include <stdint.h>

#define LOOP_TRACKS 12
#define LOOP_SLOTS 2
#define LOOP_ASSET0 0x1fc               /* slot s, generation g: asset 0x1FC + 2 s + g (stock assets count up from 0) */

enum { LS_IDLE, LS_ARMED, LS_REC, LS_CLOSING, LS_PLAY, LS_FADE };
enum { REQ_NONE, REQ_OPEN_W, REQ_CLOSE, REQ_CANCEL };   /* CLOSE: close, then open for reading */

struct loop_slot {
    uint8_t state;
    uint8_t track;
    uint8_t gen;                        /* file generation, alternates per recording */
    uint8_t req;
    uint16_t asset;
    uint8_t bars;
    uint8_t _p0;
    int32_t written;                    /* samples written while recording */
    int32_t target;                     /* file length to reach: start + the loop + XF for the crossfade */
    int32_t start;                      /* file position of the loop start (0: the recording starts on the bar) */
    uint32_t clock;                     /* samples since the recording started (keeps the bar phase) */
    int32_t armed_bar;                  /* bar index when armed / last checked */
    uint64_t lenq;                      /* loop length, 48.16 samples */
    uint64_t posq;                      /* read position, 48.16 samples */
    int32_t jump;                       /* the last wrap's jump, whole samples */
    int32_t xf_left;                    /* samples of crossfade still to do after the last wrap */
    float gain;                         /* at the end of the last block */
    float ic[4];                        /* filter state L ic1 ic2, R ic1 ic2 */
    int32_t mode0;                      /* filter mode of the last block */
    int32_t fade_left, fade_total;      /* samples */
    uint32_t reads_failed;              /* for the simulation / tests */
};

struct loop_shared {
    uint32_t magic;
    uint8_t cmd[LOOP_TRACKS];           /* M4 -> M7: 1 = the LOOP key was pressed for this track */
    uint8_t chbars[LOOP_TRACKS];        /* per track, from the channel parameters */
    int16_t chfilter[LOOP_TRACKS];
    uint8_t chfade[LOOP_TRACKS];
    uint32_t blocks;                    /* blocks processed (alive check) */
    struct loop_slot slot[LOOP_SLOTS];
};

#define LOOP_SHARED ((volatile struct loop_shared *)0x3800F000u)
#define LOOP_MAGIC 0x384f4f4cu          /* "LOO8": layout version; any other value re-initialises */

_Static_assert(sizeof(struct loop_shared) < 0x1000, "SRAM4 0x3800F000..0x38010000");
#endif

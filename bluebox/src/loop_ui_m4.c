/*
 * Looper, M4 side: the Loop view of the Track page.
 *
 * The B (Info) key on the Track page steps through the parameter blocks: block 1 (VOL PAN GAIN CUE) -> block 2
 * (FX1 FX2 OUT3&4) -> Loop view -> block 1. The Loop view shows the track's loop controls in the panel instead of the
 * stock ones: FILTER, FADE and BARS knobs (encoders 1..3) and a LOOP button.
 *
 * Stock pieces (re/bluebox.md, "Seiten und Tasten"):
 *   - the key handler 0x081354EC shows the Track page for both the Track key and B: showPage(pm, page) 0x0813AA1C
 *     (bl @0x08135718; the current page is pm + 0x34) then setBlock(page, block) 0x08133884 (bl @0x08135720). B sends
 *     the other block, the Track key block 0. loop_show notes whether the Track page was already showing (then it
 *     was B), loop_setblock steps the view.
 *   - the page's message handler (vtable 0x0814E5D4 slot 13, 0x081342AC): 0x32 = encoder (index +0x0C, delta +0x10),
 *     0x63 / 0x66 / 0x67 / 0x141 = refresh, 0xD4 = layout. loop_msg routes the encoders to our knobs in the Loop
 *     view and keeps the stock widgets hidden / ours shown after every refresh or layout.
 *   - widgets: knob ctor 0x081281E4 (0x3D0 bytes), button ctor 0x08128014 (0x1D4); setRect = vtable +0x20,
 *     setHidden = vtable +0x2C; knob: label +0xF8, param id +0x94, type +0x96, track +0x32, setParam(knob, id, value)
 *     0x08128488 (type, range and label from the parameter table), turn 0x08128D9C(knob, delta),
 *     bind(knob, id, value, 0) 0x08128BD0, text 0x08128FC8 from fmt(store, &track, id, buf) 0x08124958; button: label
 *     0x08139724, id +0xFA, track +0xF8, "sends" +0xFC, setOn 0x081280B4; addChild(page, w) 0x0813B994; parameter
 *     store 0x30018010, get(store, &track, &id, &value) 0x08122580. The page's track: u16 at +0xBC9C.
 *   - redraw: the page manager's force-redraw bytes pm + 0x38 / 0x39 (as bluelex's compressor meter does).
 *
 * Widgets and state live in SRAM4 (0x3800A000.., unused by the stock firmware; the M7 state is at 0x3800F000).
 */
#include <stdint.h>
#include "loop_shared.h"

typedef void (*ctor_fn)(void *w);
typedef void (*rect_fn)(void *w, int *rect);
typedef void (*hide_fn)(void *w, int hidden);
#define FN(a) ((a) | 1u)
#define fw_knob_ctor ((ctor_fn)FN(0x081281e4))
#define fw_btn_ctor  ((ctor_fn)FN(0x08128014))
#define fw_add_child ((void (*)(void *, void *))FN(0x0813b994))
#define fw_strncpy   ((void (*)(char *, const char *, unsigned))FN(0x0814628c))
#define fw_knob_lbl  ((void (*)(void *, int))FN(0x081288fc))
#define fw_btn_label ((void (*)(void *, const char *))FN(0x08139724))
#define fw_btn_style ((void (*)(void *, int))FN(0x081397c8))
#define fw_select    ((void (*)(void *, int))FN(0x081288d8))
#define fw_get       ((void (*)(void *, uint16_t *, uint16_t *, int32_t *))FN(0x08122580))
#define fw_bind      ((void (*)(void *, unsigned, int32_t, int))FN(0x08128bd0))
#define fw_setparam  ((void (*)(void *, unsigned, int32_t))FN(0x08128488))   /* id, type, range, label from the table */
#define fw_fmt       ((void (*)(void *, uint16_t *, unsigned, void *))FN(0x08124958))
#define fw_text      ((void (*)(void *, void *))FN(0x08128fc8))
#define fw_turn      ((int (*)(void *, int))FN(0x08128d9c))
#define fw_btn_on    ((void (*)(void *, int))FN(0x081280b4))
#define fw_show      ((void (*)(void *, void *))FN(0x0813aa1c))
#define fw_setblock  ((void (*)(void *, int))FN(0x08133884))
#define fw_page_msg  ((void (*)(void *, void *))FN(0x081342ac))
#define fw_post      ((void (*)(void *, void *))FN(0x0813ba64))   /* a message up the widget's parent chain */
#define STORE ((void *)0x30018010)

#define P_LOOP 0x103
#define P_BARS 0x104
#define P_FILTER 0x105
#define P_FADE 0x107

#define PG_TRACK 0xbc9c                     /* u16: the track the page shows */
#define PG_BLOCK 0xebbc                     /* stock block 0 / 1 */
#define KNOB(p, i) ((uint8_t *)(p) + 0xc398 + (i) * 0x3d0)
#define BTN(p, i) ((uint8_t *)(p) + 0xe298 + (i) * 0x1d4)
#define N_KNOB 8
#define N_BTN 5
#define KNOB_SIZE 0x3d0
#define BTN_SIZE 0x1d4

#define UI_KNOBS 3
struct loop_ui {
    uint32_t magic;
    uint8_t view;                           /* 1: Loop view showing */
    uint8_t was_current;                    /* the Track page was showing before this showPage */
    uint8_t built;
    uint8_t saved[N_KNOB + N_BTN];          /* stock hidden flags while the Loop view shows */
    uint8_t _p[1];
    void *pm, *page;
    int32_t acc[UI_KNOBS];                  /* encoder counts not yet turned into a step */
    char text[16];                          /* the LOOP button's text as shown */
    uint8_t lit;
    uint8_t knob[UI_KNOBS][KNOB_SIZE];      /* FILTER, BARS, FADE (encoders 1, 2, 3) */
    uint8_t btn[BTN_SIZE];                  /* LOOP */
    uint32_t btn_vt[18];                    /* the button's vtable: the stock one with our tap method */
};
#define UI ((struct loop_ui *)0x3800A000u)
#define UI_MAGIC 0x4955504cu                /* "LPUI" */
/* SRAM4 holds anything after power-on (and the last run's state after a warm reset); the stock code runs the UI loop
 * and shows pages before the Track page is built. Everything here checks UI_OK first, and loop_ui_reset (from the
 * Track page's constructor, before our widgets are built) invalidates the UI and the shared loop state. On the device L8 had
 * no such check: black screen at boot (status() followed garbage pointers; the simulation starts with SRAM4 zeroed). */
#define UI_OK (UI->magic == UI_MAGIC && UI->built == 1)
_Static_assert(sizeof(struct loop_ui) < 0x5000, "SRAM4 0x3800A000.. below the M7 state at 0x3800F000");

static const uint16_t KID[UI_KNOBS] = { P_FILTER, P_BARS, P_FADE };
static const char *const KLBL[UI_KNOBS] = { "FILTER", "BARS", "FADE" };
/* panel coordinates as the stock knobs (x, y from the bottom, w, h), in block 2's columns, so the page's frame
 * around the active block surrounds them and the encoders keep block 2's order: 1 FX1 -> FILTER (column 3 top),
 * 2 OUT3&4 -> BARS (column 3 bottom), 3 FX2 -> FADE (column 4 top) */
static const int KRECT[UI_KNOBS][4] = { { 128, 45, 48, 44 }, { 128, 1, 48, 44 }, { 176, 45, 48, 44 } };
static const int BRECT[4] = { 224, 3, 95, 86 };   /* LOOP: the whole button area */

static void rect(void *w, const int *r)
{
    int v[4] = { r[0], r[1], r[2], r[3] };
    (*(rect_fn **)w)[0x20 / 4](w, v);
}

static void hide(void *w, int h)
{
    (*(hide_fn **)w)[0x2c / 4](w, h);
}

static int hidden(void *w)
{
    return ((uint8_t *)w)[0x30];
}

static void redraw(void)
{
    if (UI->pm) {
        ((uint8_t *)UI->pm)[0x38] = 1;
        ((uint8_t *)UI->pm)[0x39] = 1;
    }
}

/* The LOOP button: the stock tap (vtable slot 6, 0x081280EC) toggles its lit state +0x96; ours (a copy of the button
 * vtable with this tap) hands the key to the M7 instead (cmd[track] in the shared state, src/loop_shared.h) and lets
 * loop_tick show the slot's state. */
#define BTN_VT 0x0814d9c4
static void loop_btn_tap(void *w, int a, int b)
{
    (void)a;
    (void)b;
    uint8_t *btn = w;
    unsigned track = *(uint16_t *)(btn + 0xf8);
    if (track < LOOP_TRACKS)
        LOOP_SHARED->cmd[track] = 1;
}

/* after the Track page's constructor (bl @0x08135BA2): build our widgets, hidden */
void loop_ui_build(void *page);
typedef void (*page_ctor_fn)(void *page);
#define fw_track_ctor ((page_ctor_fn)FN(0x08133470))
void loop_ui_reset(void);
void loop_track_ctor(void *page)
{
    loop_ui_reset();                        /* SRAM4 holds garbage (power-on) or the last run's state */
    fw_track_ctor(page);
    loop_ui_build(page);
}

void loop_ui_build(void *page)
{
    struct loop_ui *u = UI;
    u->built = 0;
    u->view = 0;
    u->was_current = 0;
    u->pm = 0;
    u->page = page;
    for (int i = 0; i < UI_KNOBS; i++) {
        uint8_t *k = u->knob[i];
        u->acc[i] = 0;
        fw_knob_ctor(k);
        fw_add_child(page, k);
        fw_strncpy((char *)k + 0xf8, KLBL[i], 0xff);
        k[0x1f7] = 0;
        fw_knob_lbl(k, 1);
        rect(k, KRECT[i]);
        hide(k, 1);
    }
    uint8_t *b = u->btn;
    fw_btn_ctor(b);
    fw_add_child(page, b);
    fw_btn_label(b, "LOOP");
    b[0x88] = 8;
    b[0xf4] = 8;
    *(uint16_t *)(b + 0xfa) = P_LOOP;
    b[0xfc] = 0;                            /* lit = busy; loop_btn_tap hands the key to the M7 */
    for (int i = 0; i < 18; i++)
        u->btn_vt[i] = ((const uint32_t *)BTN_VT)[i];
    u->btn_vt[6] = (uint32_t)loop_btn_tap | 1u;
    *(uint32_t **)b = u->btn_vt;
    fw_btn_style(b, 0x15);                  /* as REC: lit when on */
    rect(b, BRECT);
    hide(b, 1);
    u->text[0] = 0;
    u->lit = 0xff;
    u->built = 1;
    u->magic = UI_MAGIC;                    /* last: valid from here on */
}

/* from loop_track_ctor; not from loop_defs: the parameter table is set up after the Track page (Renode L9) */
void loop_ui_reset(void)
{
    UI->magic = 0;
    UI->built = 0;
    UI->view = 0;
    UI->pm = 0;
    LOOP_SHARED->magic = 0;                 /* the M7 starts its loop state afresh */
}

/* Values in the knob labels ("BARS 4", "LP 35%"): the stock knobs show only their label here.
 * Speed: a type-1 knob (whole number) moves one value per encoder count (0x0812877C: value = min + position), so BARS
 * (1..16) and FADE (0..16) went through their range ~100 times faster than VOL (~2000 counts). They are stepped here,
 * one value per STEP_COUNTS counts without the stock acceleration. FILTER: 1 % per FILTER_COUNTS counts (the stock
 * turn did not move it in the simulation). */
#define fw_snprintf ((int (*)(char *, unsigned, const char *, ...))FN(0x08146156))
#define STEP_COUNTS 96
#define FILTER_COUNTS 8

static void label(uint8_t *k, int i, int32_t v)
{
    char *l = (char *)k + 0xf8;
    if (KID[i] == P_FILTER)
        fw_snprintf(l, 16, v < 0 ? "LP %d%%" : v > 0 ? "HP %d%%" : "FILTER", (v < 0 ? -v : v) / 10);
    else if (KID[i] == P_FADE && v == 0)
        fw_snprintf(l, 16, "FADE OFF");
    else
        fw_snprintf(l, 16, "%s %d", KLBL[i], (int)v);
    fw_knob_lbl(k, 1);
}

static void set_value(int i, uint16_t track, int32_t v)
{
    uint8_t *k = UI->knob[i];
    struct { uint16_t code, _p; uint32_t sender; uint16_t track, _p1; uint32_t id; int32_t value; uint32_t _p2; } m;
    fw_bind(k, KID[i], v, 0);
    m.code = 0x6e;
    m._p = 0;
    m.sender = 0x0814b0f0;
    m.track = track;
    m._p1 = 0;
    m.id = KID[i];
    m.value = v;
    m._p2 = 0;
    fw_post(k, &m);
    label(k, i, v);
}

static void turn(void *page, int i, int32_t delta)
{
    struct loop_ui *u = UI;
    uint16_t track = *(uint16_t *)((uint8_t *)page + PG_TRACK), t = track, id = KID[i];
    int32_t v = 0;
    fw_get(STORE, &t, &id, &v);
    int32_t lo = KID[i] == P_BARS ? 1 : KID[i] == P_FILTER ? -1000 : 0, hi = KID[i] == P_FILTER ? 1000 : 16;
    int32_t per = KID[i] == P_FILTER ? FILTER_COUNTS : STEP_COUNTS, unit = KID[i] == P_FILTER ? 10 : 1;
    if ((u->acc[i] > 0 && delta < 0) || (u->acc[i] < 0 && delta > 0))
        u->acc[i] = 0;                      /* direction changed: start counting again */
    u->acc[i] += delta;
    int32_t steps = u->acc[i] / per;
    if (!steps)
        return;
    u->acc[i] -= steps * per;
    int32_t n = v + steps * unit;
    n = n < lo ? lo : n > hi ? hi : n;
    if (n != v)
        set_value(i, track, n);
    redraw();
}

/* values of the page's track into our widgets */
static void refresh(void *page)
{
    struct loop_ui *u = UI;
    uint16_t track = *(uint16_t *)((uint8_t *)page + PG_TRACK);
    for (int i = 0; i < UI_KNOBS; i++) {
        uint8_t *k = u->knob[i];
        uint16_t t = track, id = KID[i];
        int32_t v = 0;
        fw_get(STORE, &t, &id, &v);
        *(uint16_t *)(k + 0x32) = track;
        if (*(uint16_t *)(k + 0x94) != id)
            fw_setparam(k, id, v);          /* first time: type, range and label from the parameter table */
        fw_bind(k, id, v, 0);
        label(k, i, v);
        uint8_t txt[0x70];                  /* the stock update prepares its text buffer like this */
        txt[0] = 1;
        txt[2] = 0;
        txt[3] = 1;
        txt[4] = 0;
        t = track;
        fw_fmt(STORE, &t, id, txt);
        fw_text(k, txt);
    }
    *(uint16_t *)(u->btn + 0xf8) = track;
    u->lit = 0xff;                          /* loop_tick shows the track's slot */
}

/* hide the stock widgets and show ours, or the other way round */
static void apply(void *page)
{
    struct loop_ui *u = UI;
    if (!UI_OK)
        return;
    for (int i = 0; i < N_KNOB; i++)
        if (u->view && !hidden(KNOB(page, i)))
            hide(KNOB(page, i), 1);
    for (int i = 0; i < N_BTN; i++)
        if (u->view && !hidden(BTN(page, i)))
            hide(BTN(page, i), 1);
    for (int i = 0; i < UI_KNOBS; i++) {
        hide(u->knob[i], !u->view);
        fw_select(u->knob[i], u->view);
    }
    hide(u->btn, !u->view);
    redraw();
}

static void enter(void *page)
{
    struct loop_ui *u = UI;
    for (int i = 0; i < N_KNOB; i++)
        u->saved[i] = (uint8_t)hidden(KNOB(page, i));
    for (int i = 0; i < N_BTN; i++)
        u->saved[N_KNOB + i] = (uint8_t)hidden(BTN(page, i));
    u->view = 1;
    refresh(page);
    apply(page);
}

static void leave(void *page)
{
    struct loop_ui *u = UI;
    if (!u->view)
        return;
    u->view = 0;
    for (int i = 0; i < N_KNOB; i++)
        hide(KNOB(page, i), u->saved[i]);
    for (int i = 0; i < N_BTN; i++)
        hide(BTN(page, i), u->saved[N_KNOB + i]);
    apply(page);
}

/* bl @0x08135718: showPage(pm, page) */
void loop_show(void *pm, void *page)
{
    struct loop_ui *u = UI;
    u->pm = pm;
    u->was_current = *(void **)((uint8_t *)pm + 0x34) == page;
    if (!u->was_current && UI_OK && page == u->page)
        leave(page);                        /* the Track page opens fresh: stock view */
    fw_show(pm, page);
}

/* bl @0x08135720: setBlock(page, block) */
void loop_setblock(void *page, int block)
{
    struct loop_ui *u = UI;
    if (!UI_OK || page != u->page || !u->was_current) {
        fw_setblock(page, block);
        return;
    }
    /* B on the Track page: block 1 -> block 2 -> Loop view -> block 1 */
    if (u->view) {
        leave(page);
        fw_setblock(page, 0);
    } else if (*(int32_t *)((uint8_t *)page + PG_BLOCK) == 1) {
        enter(page);
    } else {
        fw_setblock(page, block);
    }
}

/* the Track page's message handler (vtable slot 13) */
void loop_msg(void *page, void *msg)
{
    struct loop_ui *u = UI;
    uint16_t code = *(uint16_t *)msg;
    if (UI_OK && u->view && page == u->page && code == 0x32) {
        int enc = *(int16_t *)((uint8_t *)msg + 0x0c), delta = *(int32_t *)((uint8_t *)msg + 0x10);
        if (enc >= 0 && enc < UI_KNOBS)
            turn(page, enc, delta);
        return;
    }
    fw_page_msg(page, msg);
    if (UI_OK && u->view && page == u->page &&
        (code == 0x63 || code == 0x66 || code == 0x67 || code == 0x141 || code == 0xd4)) {
        refresh(page);
        apply(page);                        /* a refresh / layout may have shown stock widgets again */
    }
}

/* ---- the loop files and the state on the LOOP button ----
 * The M7 asks for its files in slot.req (src/loop_shared.h); loop_tick (on the UI loop's redraw check,
 * bl 0x0813AA9E @0x081351A6) queues them at the M4 stream manager (object 0x3000B778) as the stock recorder does
 * (simulation trace of a stock take): open for writing 0x0810A840(sm, asset, path, channels 2, 1) (command 9, when
 * a track is armed), close 0x0810A8FC(sm, asset) (command 12: header, length into the cache), open for reading
 * 0x0810A630(sm, asset, path, 1, 1) (command 7). Opening an asset for writing first releases whatever it had open
 * (0x0810B8B0). Files: LOOPA0.WAV / LOOPA1.WAV (slot 1), LOOPB0.WAV / LOOPB1.WAV (slot 2) in the card's root,
 * 24-bit stereo; each recording overwrites the older file of its slot. */
#define SM ((void *)0x3000b778)
#define fw_open_w ((void (*)(void *, unsigned, const char *, unsigned, unsigned))FN(0x0810a840))
#define fw_close ((void (*)(void *, unsigned))FN(0x0810a8fc))
#define fw_open_r ((void (*)(void *, unsigned, const char *, unsigned, unsigned))FN(0x0810a630))
#define fw_redraw ((int (*)(void *, void *))FN(0x0813aa9e))

static const char *const PATH[LOOP_SLOTS][2] = { { "LOOPA0.WAV", "LOOPA1.WAV" }, { "LOOPB0.WAV", "LOOPB1.WAV" } };

static void files(void)
{
    volatile struct loop_shared *s = LOOP_SHARED;
    if (s->magic != LOOP_MAGIC)
        return;
    for (unsigned i = 0; i < LOOP_SLOTS; i++) {
        volatile struct loop_slot *sl = &s->slot[i];
        unsigned req = sl->req, asset = sl->asset;
        const char *path = PATH[i][sl->gen & 1];
        if (req == REQ_OPEN_W)
            fw_open_w(SM, asset, path, 2, 1);
        else if (req == REQ_CLOSE) {
            fw_close(SM, asset);
            fw_open_r(SM, asset, path, 1, 1);
        } else if (req == REQ_CANCEL)
            fw_close(SM, asset);
        if (req != REQ_NONE)
            sl->req = REQ_NONE;
    }
}

/* the LOOP button shows the track's slot: LOOP / ARMED / REC 2/4 / WAIT / PLAY 3/4 / FADE (lit while busy) */
static void status(void)
{
    struct loop_ui *u = UI;
    if (!UI_OK || !u->view || !u->page)
        return;
    volatile struct loop_shared *s = LOOP_SHARED;
    unsigned track = *(uint16_t *)((uint8_t *)u->page + PG_TRACK);
    char txt[16];
    int lit = 0;
    fw_snprintf(txt, sizeof txt, "LOOP");
    if (s->magic == LOOP_MAGIC)
        for (unsigned i = 0; i < LOOP_SLOTS; i++) {
            volatile struct loop_slot *sl = &s->slot[i];
            if (sl->state == LS_IDLE || sl->track != track)
                continue;
            unsigned bars = sl->bars ? sl->bars : 1;
            uint32_t len = (uint32_t)(sl->lenq >> 16), bar = len / bars ? len / bars : 1;
            lit = 1;
            if (sl->state == LS_ARMED)
                fw_snprintf(txt, sizeof txt, "ARMED");
            else if (sl->state == LS_REC) {
                unsigned b = (unsigned)(sl->written - sl->start) / bar + 1;
                fw_snprintf(txt, sizeof txt, "REC %u/%u", b > bars ? bars : b, bars);
            } else if (sl->state == LS_CLOSING)
                fw_snprintf(txt, sizeof txt, "WAIT");
            else if (sl->state == LS_PLAY) {
                unsigned b = (uint32_t)(sl->posq >> 16) / bar + 1;
                fw_snprintf(txt, sizeof txt, "PLAY %u/%u", b > bars ? bars : b, bars);
            } else {
                fw_snprintf(txt, sizeof txt, "FADE");
                continue;                   /* a fading loop: an armed / new one of the track wins */
            }
            break;
        }
    int changed = lit != u->lit;
    for (unsigned i = 0; i < sizeof txt && !changed; i++) {
        changed = txt[i] != u->text[i];
        if (!txt[i])
            break;
    }
    if (!changed)
        return;
    for (unsigned i = 0; i < sizeof txt; i++)
        u->text[i] = txt[i];
    u->lit = (uint8_t)lit;
    fw_btn_label(u->btn, u->text);
    fw_btn_on(u->btn, lit);
    redraw();
}

int loop_tick(void *pm, void *arg)
{
    files();
    status();
    return fw_redraw(pm, arg);
}

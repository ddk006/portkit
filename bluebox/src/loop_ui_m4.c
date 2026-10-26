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
    uint8_t knob[UI_KNOBS][KNOB_SIZE];      /* FILTER, BARS, FADE (encoders 1, 2, 3) */
    uint8_t btn[BTN_SIZE];                  /* LOOP */
    uint32_t btn_vt[18];                    /* the button's vtable: the stock one with our tap method */
};
#define UI ((struct loop_ui *)0x3800A000u)
#define UI_MAGIC 0x4955504cu                /* "LPUI" */
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

/* The stock button sends its parameter itself only with flag +0xFC set, and that flag also inverts the display (lit =
 * value 0). Ours keeps +0xFC = 0 (lit = on) and gets a copy of the button vtable whose tap method (slot 6,
 * 0x081280EC: toggles the state +0x96) is followed by our own 0x6E message: track +0xF8, id +0xFA, the new state. */
#define BTN_VT 0x0814d9c4
#define fw_btn_tap ((void (*)(void *, int, int))FN(0x081280ec))
#define fw_post ((void (*)(void *, void *))FN(0x0813ba64))
static void loop_btn_tap(void *w, int a, int b)
{
    uint8_t *btn = w;
    uint8_t before = btn[0x96];
    fw_btn_tap(w, a, b);
    if (btn[0x96] == before)
        return;                             /* not a toggle (disabled, ...) */
    struct { uint16_t code, _p; uint32_t sender; uint16_t track, _p1; uint32_t id; int32_t value; uint32_t _p2; } m;
    m.code = 0x6e;
    m._p = 0;
    m.sender = 0x0814b0f0;                  /* as the stock senders (literal 0x08128154) */
    m.track = *(uint16_t *)(btn + 0xf8);
    m._p1 = 0;
    m.id = *(uint16_t *)(btn + 0xfa);
    m.value = btn[0x96];
    m._p2 = 0;
    fw_post(w, &m);
}

/* after the Track page's constructor (bl @0x08135BA2): build our widgets, hidden */
void loop_ui_build(void *page);
typedef void (*page_ctor_fn)(void *page);
#define fw_track_ctor ((page_ctor_fn)FN(0x08133470))
void loop_track_ctor(void *page)
{
    fw_track_ctor(page);
    loop_ui_build(page);
}

void loop_ui_build(void *page)
{
    struct loop_ui *u = UI;
    u->magic = UI_MAGIC;
    u->view = 0;
    u->was_current = 0;
    u->pm = 0;
    u->page = page;
    for (int i = 0; i < UI_KNOBS; i++) {
        uint8_t *k = u->knob[i];
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
    b[0xfc] = 0;                            /* lit = on; loop_btn_tap sends the value */
    for (int i = 0; i < 18; i++)
        u->btn_vt[i] = ((const uint32_t *)BTN_VT)[i];
    u->btn_vt[6] = (uint32_t)loop_btn_tap | 1u;
    *(uint32_t **)b = u->btn_vt;
    fw_btn_style(b, 0x15);                  /* as REC: lit when on */
    rect(b, BRECT);
    hide(b, 1);
    u->built = 1;
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
        uint8_t txt[0x70];                  /* the stock update prepares its text buffer like this */
        txt[0] = 1;
        txt[2] = 0;
        txt[3] = 1;
        txt[4] = 0;
        t = track;
        fw_fmt(STORE, &t, id, txt);
        fw_text(k, txt);
    }
    uint16_t t = track, id = P_LOOP;
    int32_t on = 0;
    fw_get(STORE, &t, &id, &on);
    *(uint16_t *)(u->btn + 0xf8) = track;
    fw_btn_on(u->btn, on != 0);
}

/* hide the stock widgets and show ours, or the other way round */
static void apply(void *page)
{
    struct loop_ui *u = UI;
    if (!u->built)
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
    if (!u->was_current && u->built && page == u->page)
        leave(page);                        /* the Track page opens fresh: stock view */
    fw_show(pm, page);
}

/* bl @0x08135720: setBlock(page, block) */
void loop_setblock(void *page, int block)
{
    struct loop_ui *u = UI;
    if (!u->built || page != u->page || !u->was_current) {
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
    if (u->built && u->view && page == u->page && code == 0x32) {
        int enc = *(int16_t *)((uint8_t *)msg + 0x0c), delta = *(int32_t *)((uint8_t *)msg + 0x10);
        if (enc >= 0 && enc < UI_KNOBS) {
            fw_turn(u->knob[enc], (int16_t)delta);
            redraw();
        }
        return;
    }
    fw_page_msg(page, msg);
    if (u->built && u->view && page == u->page &&
        (code == 0x63 || code == 0x66 || code == 0x67 || code == 0x141 || code == 0xd4)) {
        refresh(page);
        apply(page);                        /* a refresh / layout may have shown stock widgets again */
    }
}

/* ---- recording that stops after BARS bars ----
 * The REC key reaches the app as message 0x69 (STOP 0x6C); the app hands codes below 0x7B to the page manager
 * (bl 0x0813AA90 @0x08135B24). loop_app_msg looks at REC first: if a track has Rec Enable (0x162) and LOOP on, the
 * recording is to end BARS bars (+ a 16th for the loop crossfade) after the next bar line (at least a 16th ahead). The
 * stock recorder starts at once as usual; the M7 loops from that bar line (src/loop_m7.c, autostart). Rec Enable of
 * those tracks is switched off with the take: the player does not play clips while it is set (0x162 -> player +0x96,
 * render branch @0x08057CA4), so the loop would stay silent.
 * The transport's Punch In / Out are not used: applying them via 0x08126524 while the transport runs made the recorder
 * register a take without writing its file (simulation: no WAV on the card, take start at the REC press).
 * Transport position: ticks (960 PPQ) published by the M7 at 0x300478B0. */
#define fw_pm_msg ((void (*)(void *, void *))FN(0x0813aa90))
#define TICKS (*(volatile int32_t *)0x300478b0)
#define P_RECEN 0x162
#define BAR 3840
#define SIXTEENTH 240

struct rec_state {
    uint32_t magic;
    uint32_t pending;                       /* a loop recording runs: finish its take at out */
    int32_t out;                            /* ticks */
    uint32_t tracks;                        /* bit n: track n records into a loop (Rec Enable and LOOP) */
    int32_t err_until;                      /* ticks: end of the window after the take (see loop_tick) */
    uint32_t err0;                          /* stream error count before the take ended */
};
#define RS ((struct rec_state *)0x3800EF00u)
#define RS_MAGIC 0x43455250u                 /* "PREC" */

static int32_t pget(unsigned set, unsigned id)
{
    uint16_t t = (uint16_t)set, i = (uint16_t)id;
    int32_t v = 0;
    fw_get(STORE, &t, &i, &v);
    return v;
}

static void rec_for_loop(void)
{
    int bars = 0;
    uint32_t tracks = 0;
    for (unsigned tr = 0; tr < 12; tr++)
        if (pget(tr, P_RECEN) && pget(tr, P_LOOP)) {
            if (!bars)
                bars = pget(tr, P_BARS);    /* the first such track sets the length */
            tracks |= 1u << tr;
        }
    RS->magic = RS_MAGIC;
    RS->pending = 0;
    RS->tracks = tracks;
    RS->err_until = 0;
    if (bars <= 0)
        return;                             /* a normal recording */
    if (bars > 16)
        bars = 16;
    int32_t now = TICKS, in = (now / BAR + 1) * BAR;
    if (in - now < SIXTEENTH)
        in += BAR;
    RS->out = in + bars * BAR + SIXTEENTH;
    RS->pending = 1;
}

/* The stock finishes a take only at STOP (0x081272CC(store, 0) = record off: with a take running, flag store + 0x743B,
 * 0x08127150 sets its end, queues closing the file and sends the clip to the M7 via 0x08125040; the transport is not
 * touched, STOP stops it separately). So the new clip would not exist while the transport runs on. loop_tick, on the UI
 * loop's redraw check (bl 0x0813AA9E @0x081351A6), switches record off once the transport is past out; the M7 then
 * starts the clip at once (src/loop_m7.c, autostart). */
#define fw_redraw ((int (*)(void *, void *))FN(0x0813aa9e))
#define fw_setrec ((void (*)(void *, int))FN(0x081272cc))
#define TAKE_RUNNING (*(volatile uint8_t *)((uint8_t *)STORE + 0x743b))
/* Stream errors shown in the header (0x08122C08 -> "ERROR %d" / "REC ERROR"): the cache's count at +0x362C plus a
 * second one. Finishing a take while the transport runs makes the Mixer node (0x08050564, which reads the recording
 * track at the write position) miss about 8 ticks of reads while the M4 streamer registers the file again (simulation:
 * 7 errors at ticks 8024..8032 after a take ending at 7920; the stock finishes takes only at STOP, so it has none).
 * Those are counted back for an 8th note after the take; errors outside that window stay. */
#define STREAM_ERRS (*(volatile uint32_t *)(0x30044270u + 0x362c))
#define ERR_WINDOW 480
#define fw_store_msg ((void (*)(void *, void *))FN(0x08126e8c))   /* where the UI's 0x6E messages end (0x0813A9C8) */

static void pset(unsigned track, unsigned id, int32_t value)
{
    struct { uint16_t code, _p; uint32_t sender; uint16_t track, _p1; uint32_t id; int32_t value; uint32_t _p2; } m;
    m.code = 0x6e;
    m._p = 0;
    m.sender = 0x0814b0f0;
    m.track = (uint16_t)track;
    m._p1 = 0;
    m.id = id;
    m.value = value;
    m._p2 = 0;
    fw_store_msg(STORE, &m);
}

int loop_tick(void *pm, void *arg)
{
    if (RS->magic == RS_MAGIC && RS->pending) {
        if (!TAKE_RUNNING) {
            if (TICKS >= RS->out)
                RS->pending = 0;            /* stopped or finished another way */
        } else if (TICKS >= RS->out) {
            RS->pending = 0;
            RS->err0 = STREAM_ERRS;
            RS->err_until = TICKS + ERR_WINDOW;
            fw_setrec(STORE, 0);
            for (unsigned tr = 0; tr < 12; tr++)
                if (RS->tracks >> tr & 1)
                    pset(tr, P_RECEN, 0);
        }
    }
    if (RS->magic == RS_MAGIC && RS->err_until) {
        if (TICKS < RS->err_until && TICKS >= RS->err_until - ERR_WINDOW - BAR) {
            if (STREAM_ERRS > RS->err0)
                STREAM_ERRS = RS->err0;
        } else
            RS->err_until = 0;
    }
    return fw_redraw(pm, arg);
}

void loop_app_msg(void *pm, void *msg)
{
    uint16_t code = *(uint16_t *)msg;
    if (code == 0x69)
        rec_for_loop();
    fw_pm_msg(pm, msg);
}

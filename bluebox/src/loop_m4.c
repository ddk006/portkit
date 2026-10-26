/*
 * Looper, M4 side: two channel parameters.
 *   0x103 LOOP  (toggle 0/1, key "chloop")       the track's clip loops (one of two loop slots)
 *   0x104 BARS  (1..16, key "chloopbars")        loop length in bars
 * Ids 0x102..0x105 are undefined in the M4 table and appear nowhere in either core (re/bluebox.md); bluelex uses
 * 0x3a/0x3d/0x4a/0x4b/0x43/0x59, so the two mods stay apart.
 *
 * loop_defs replaces the table's last definition (bl FUN_08135E08 @0x0813714A, id 0x117) and adds ours after it:
 * FUN_08135E08(table, id, type, label, min, max, key). loop_set_add replaces one add of the channel parameter set
 * (FUN_0812060C case 1, bl FUN_081205E2 @0x0812087C, id 0x2A) and adds ours after it, so they are kept per track,
 * saved with the project and sent to the track's queue on the M7 (src/loop_m7.c, loop_process).
 */
typedef void (*def_fn)(void *table, unsigned id, unsigned type, const char *label, int min, int max, const char *key);
typedef void (*add_fn)(void *set, unsigned id, int value);
#define fw_def ((def_fn)0x08135e09)
#define fw_add ((add_fn)0x081205e3)
#define T_INT 1                  /* a plain whole number (stock: Channel 1..16, BPM) */
#define T_TOGGLE 4               /* stock buttons (Mute, Solo, Rec Enble) */
#define P_LOOP 0x103
#define P_BARS 0x104
#define BARS_DEFAULT 4

void loop_defs(void *table, unsigned id, unsigned type, const char *label, int min, int max, const char *key)
{
    fw_def(table, id, type, label, min, max, key);
    fw_def(table, P_LOOP, T_TOGGLE, "Loop:", 0, 1, "chloop");
    fw_def(table, P_BARS, T_INT, "Bars:", 1, 16, "chloopbars");
}

void loop_set_add(void *set, unsigned id, int value)
{
    fw_add(set, id, value);
    fw_add(set, P_LOOP, 0);
    fw_add(set, P_BARS, BARS_DEFAULT);
}

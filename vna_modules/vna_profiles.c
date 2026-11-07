/*
 * Quick antenna measurement profiles + quick S11 calibration (LoRa bands)
 *
 * This file is included from ui.c (like the other vna_modules), so it can use
 * the static menu framework of ui.c. Enabled by __VNA_LORA_PROFILES__.
 * Plot side (autoscale, LORA INFO block, REF, TUNE screen) is in vna_lora_plot.c (plot.c),
 * full screen / action bar drawing in vna_lora_screens.c.
 *
 * Profiles (WIDE, SURVEY, 433, 869, 915 + CUSTOM):
 *   - each profile sets sweep range + points, an S11-only display (SWR bold + RL, or
 *     SWR + Smith, or all of SWR/RL/X/Smith: VIEW), autoscaled, the LORA INFO block,
 *     marker 1 tracking the SWR minimum and markers on the ISM band edges (WIDE/SURVEY:
 *     on the 433/868/915 band centres). Everything else is reset.
 *   - each profile owns a fixed save slot (1 = SURVEY, 3..6 = WIDE/433/869/915, 2 = CUSTOM,
 *     0 = stock power on slot untouched); a calibration made over exactly the profile
 *     range/points is recalled from it.
 *   - last used profile/custom is stored in config. At power on a chooser on the splash
 *     preselects it and starts it after 3 s; first boot (or new firmware build) waits.
 * Profile screen action bar (instead of the frequency line), one tap each:
 *   [PROFILE] full screen chooser  [CAL] QUICK CAL wizard (OPEN/SHORT/LOAD -> profile slot)
 *   [TUNE] big digit tune screen   [REF] store S11 reference (hold: clear)   [HOLD] [S21]: later
 *   Info block: drag to move, tap a moved block = auto position, tap its legend line = VIEW.
 *   Jog: left/right moves M1 (FREE), long push = M1 TRACK/FREE, short push = stock menu.
 * Shell:
 *   - "profile"              list: name start stop points slot cal(0/1) active(0/1),
 *                            then "custom ...", "boot <name> ask=<0|1>", "view <name>"
 *   - "profile <name>"       apply profile (wide|433|869|915|survey|custom), prints its line
 *   - "profile view [rl|smith|all]"  show / set the profile VIEW (SWR+RL, SWR+Smith, all)
 *   - "profile info"         res/bw/q/worst/flag/dip/ref/tune lines (see vna_lora_plot.c)
 *   - "profile ref [store|clear]", "profile tune [on|off]"
 *
 * This is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3, or (at your option) any later version.
 */
#ifdef __VNA_LORA_PROFILES__

typedef struct {
  const char *name;     // shell name
  const char *title;    // screen title
  const char *label;    // menu label (name + range)
  const char *shortname;// chooser / action bar name
  const char *range;    // chooser range text
  freq_t start, stop;   // sweep range
  int8_t band;          // ISM band in lora_bands[] (shading, edge markers, pass/fail), -1 = all (WIDE)
  uint8_t slot;         // save slot of the profile calibration
  freq_t target;        // band centre used for the offset readout (0 = none)
} lora_profile_t;

// ISM bands (edge markers, shading)
static const freq_t lora_bands[][2] = {
  {433050000U, 434790000U},   // 433 ISM (EU/ITU region 1)
  {863000000U, 870000000U},   // 868 EU SRD
  {902000000U, 928000000U},   // 915 US ISM
};
// WIDE: markers on the band centres
static const freq_t lora_wide_mk[LORA_MK_MAX] = {433920000U, 868000000U, 915000000U};

// Shell names list, must be in the same order as lora_profiles[]
#define LORA_PROFILE_NAMES "wide|433|869|915|survey"
// Band profiles: centre +-~10% (dip of a detuned antenna stays in view), 401 points
// (225..450 kHz step, 8..58 points inside the band). WIDE: find a far detuned antenna.
// SURVEY: unknown antenna, 50 MHz - 1.5 GHz, lists the dips.
// Save slots: 0 = stock power on slot (untouched), 1 = SURVEY, 2 = CUSTOM, 3..6 = WIDE/433/869/915
static const lora_profile_t lora_profiles[] = {
  {"wide",   "WIDE",            "WIDE\n300M-1G",    "WIDE",   "300 - 1000 MHz",  300000000U, 1000000000U, -1, 3,          0},
  {"433",    "433 ISM",         "433 ISM\n389-479M", "433",    "389 - 479 MHz",   388920000U,  478920000U,  0, 4, 433920000U},
  {"869",    "869 EU",          "869 EU\n779-959M",  "869",    "779 - 959 MHz",   779000000U,  959000000U,  1, 5, 869000000U},
  {"915",    "915 US",          "915 US\n825-1005M", "915",    "825 - 1005 MHz",  825000000U, 1005000000U,  2, 6, 915000000U},
  {"survey", "SURVEY",          "SURVEY\n50M-1.5G",  "SURVEY", "50 - 1500 MHz",    50000000U, 1500000000U, -1, 1,          0},
};
#define LORA_PROFILE_COUNT ((int)ARRAY_COUNT(lora_profiles))
#define LORA_POINTS        POINTS_COUNT_DEFAULT
#define LORA_CUSTOM        LORA_PROFILE_COUNT   // choice index used for CUSTOM
#define lora_slot(i)       (lora_profiles[i].slot)
#define lora_custom_slot   2

#include "vna_lora_screens.c"

// Persistent state in a spare config byte: bits 0..3 = last used index + 1, bits 4..5 = VIEW,
// bit 7 = do not ask at boot
#define lora_cfg           config._reserved[0]
#define LORA_CFG_NO_ASK    0x80
#define LORA_CFG_VIEW_SHIFT 4
#define LORA_CFG_VIEW_MASK  (3<<LORA_CFG_VIEW_SHIFT)
// VIEW: which of the profile traces (0 SWR, 1 LOGMAG/RL, 2 X, 3 Smith) are shown
enum {LORA_VIEW_RL = 0, LORA_VIEW_SMITH, LORA_VIEW_ALL, LORA_VIEW_COUNT};
static const uint8_t lora_view_traces[LORA_VIEW_COUNT] = {0x03, 0x09, 0x0F};
static const char *lora_view_names[LORA_VIEW_COUNT]    = {"SWR+RL", "SWR+SMITH", "ALL"};
#define LORA_VIEW_SHELL    "rl|smith|all"
// Build stamp of the firmware that last stored a power on choice (spare config bytes 1..2):
// differs after flashing a new build -> first boot -> chooser waits for the user
#define lora_cfg_stamp_lo  config._reserved[1]
#define lora_cfg_stamp_hi  config._reserved[2]

#if SAVEAREA_MAX < 7
#error "LoRa profiles need 7 save slots (0 stock, 1 SURVEY, 2 CUSTOM, 3..6 bands)"
#endif
#if TRACES_MAX != 4
#error "LoRa profiles display needs 4 traces"
#endif

static bool lora_autoscale_on = true;

static int lora_active(void) {
  for (int i = 0; i < LORA_PROFILE_COUNT; i++)
    if (frequency0 == lora_profiles[i].start && frequency1 == lora_profiles[i].stop) return i;
  return -1;
}

// Applied calibration was made over exactly this profile range/points
static bool lora_cal_ok(const lora_profile_t *p) {
  return (cal_status & CALSTAT_APPLY) && cal_frequency0 == p->start && cal_frequency1 == p->stop
      && cal_sweep_points == sweep_points;
}

// Profile slot holds a calibration made over exactly the profile range/points
static bool lora_has_cal(int i) {
  const lora_profile_t *p = &lora_profiles[i];
  const properties_t *sp = get_properties(lora_slot(i));
  return sp && (sp->_cal_status & (CALSTAT_OPEN|CALSTAT_SHORT|CALSTAT_LOAD))
            && sp->_cal_frequency0 == p->start && sp->_cal_frequency1 == p->stop
            && sp->_cal_sweep_points == LORA_POINTS;
}

// Screen info for the plot side (vna_lora_plot.c), false if the sweep range is not a profile
bool lora_profile_screen(lora_screen_t *s) {
  int i = lora_active();
  if (i < 0) return false;
  const lora_profile_t *p = &lora_profiles[i];
  s->title  = p->title;
  s->target = p->target;
  if (p->band >= 0) {s->band = &lora_bands[p->band]; s->nband = 1; s->mk = NULL;}
  else              {s->band = lora_bands; s->nband = ARRAY_COUNT(lora_bands); s->mk = lora_wide_mk;}
  s->cal = lora_cal_ok(p) ? LORA_CAL_OK : (cal_status & CALSTAT_APPLY) ? LORA_CAL_RANGE : LORA_CAL_NONE;
  return true;
}

static int lora_view(void) {
  int v = (lora_cfg & LORA_CFG_VIEW_MASK) >> LORA_CFG_VIEW_SHIFT;
  return v < LORA_VIEW_COUNT ? v : LORA_VIEW_RL;
}

// Show the VIEW trace set (profile layout: 0 SWR, 1 LOGMAG, 2 X, 3 Smith)
static void lora_view_apply(void) {
  uint8_t mask = lora_view_traces[lora_view()];
  for (int t = 0; t < TRACES_MAX; t++)
    trace[t].enabled = (mask >> t) & 1;
  current_trace = 0;
  request_to_redraw(REDRAW_AREA | REDRAW_MARKER);
}

static void lora_view_set(int v) {
  lora_cfg = (lora_cfg & ~LORA_CFG_VIEW_MASK) | (v << LORA_CFG_VIEW_SHIFT);
  config_save();
  if (lora_active() >= 0) lora_view_apply();
}

static void lora_marker_at(int m, freq_t f) {
  markers[m].enabled = TRUE;
  int idx = (float)(f - frequency0) * (sweep_points - 1) / (float)(frequency1 - frequency0) + 0.5f;
  set_marker_index(m, idx);
}

// Reset all marker/trace leftovers, then M1 (SWR minimum, tracking) + band markers M2..
static void lora_markers_reset(const lora_profile_t *p) {
  // Erase old marker plates, disable all markers
  for (int m = 0; m < MARKERS_MAX; m++) {
    if (markers[m].enabled) request_to_draw_marker(markers[m].index);
    markers[m].enabled = FALSE;
  }
  // Stored (reference) traces off
#if STORED_TRACES > 0
  uint8_t st = getStoredTraces();
  for (int i = 0; i < STORED_TRACES; i++)
    if (st & (1<<i)) toogleStoredTrace(i);
#endif
  // No delta / reference marker, tracking on, jog moves the marker
  props_mode = (props_mode & ~TD_MARKER_DELTA) | TD_MARKER_TRACK;
  previous_marker = MARKER_INVALID;
  active_marker   = 0;
  select_lever_mode(LM_MARKER);
  // M1: SWR minimum (tracked after each sweep), start at centre
  markers[0].enabled = TRUE;
  set_marker_index(0, sweep_points / 2);
  // M2.. band edges, WIDE: band centres
  if (p->band >= 0) {
    lora_marker_at(1, lora_bands[p->band][0]);
    lora_marker_at(2, lora_bands[p->band][1]);
  } else {
    for (int m = 0; m < LORA_MK_MAX; m++)
      lora_marker_at(1 + m, lora_wide_mk[m]);
  }
}

// Store last used profile/custom for power on (writes config to flash only if changed)
static void lora_remember(int idx) {
  if ((lora_cfg & 0x0F) == idx + 1) return;
  lora_cfg = (lora_cfg & ~0x0F) | (idx + 1);
  config_save();
}

static bool lora_custom_valid(void) {
  return get_properties(lora_custom_slot) != NULL;
}

// Apply profile, return true if the profile calibration was recalled
static bool lora_profile_apply(int idx, bool remember) {
  const lora_profile_t *p = &lora_profiles[idx];
  // Recall slot calibration (restores sweep range/points/cal) if it matches
  bool recalled = lora_has_cal(idx) && load_properties(lora_slot(idx)) == 0;
  // Sweep points + range
  set_sweep_points(LORA_POINTS);
  set_sweep_frequency(ST_START, p->start);
  set_sweep_frequency(ST_STOP,  p->stop);
  if (p->band >= 0) FREQ_CENTERSPAN();  // show "CENTER xxx SPAN yyy" for band profiles
  // Frequency domain
  props_mode&= ~DOMAIN_MODE;
  plot_set_measure_mode(MEASURE_LORA_INFO);
  // S11 only display, scale/ref are initial values, autoscale adjusts them after each sweep
  static const trace_t lora_trace[TRACES_MAX] = {
    { TRUE, TRC_SWR,    0, MS_RX,  0.5f, 0        },  // SWR 1..5
    { TRUE, TRC_LOGMAG, 0, MS_RX,  5.0f, NGRIDY   },  // 0..-40 dB (return loss)
    { TRUE, TRC_X,      0, MS_RX, 50.0f, NGRIDY/2 },  // X +-200 Ohm, zero crossing = resonance
    { TRUE, TRC_SMITH,  0, MS_RX,  1.0f, 0        },
  };
  memcpy(trace, lora_trace, sizeof(lora_trace));
  lora_view_apply();                                  // VIEW: enabled traces, current = SWR
  lora_autoscale_mask = lora_autoscale_on ? 0x0F : 0;
  lora_markers_reset(p);
  // Marker search = minimum (global config option)
  apply_VNA_mode(VNA_MODE_SEARCH, VNA_MODE_SET);
  if (remember) lora_remember(idx);
  // No REDRAW_CLRSCR: may be called from shell while the menu is open
  request_to_redraw(REDRAW_AREA | REDRAW_PLOT | REDRAW_MARKER | REDRAW_FREQUENCY | REDRAW_CAL_STATUS | REDRAW_BACKUP);
  return recalled;
}

// CUSTOM: recall the user's own saved setup as is (no autoscale), false if slot empty
static bool lora_custom_apply(bool remember) {
  if (remember) lora_remember(LORA_CUSTOM);
  if (!lora_custom_valid() || load_properties(lora_custom_slot) != 0) return false;
  lora_autoscale_mask = 0;
  request_to_redraw(REDRAW_AREA | REDRAW_PLOT | REDRAW_MARKER | REDRAW_FREQUENCY | REDRAW_CAL_STATUS | REDRAW_BACKUP);
  return true;
}

static void lora_tune_exit(void);
static void lora_select(int idx, bool remember) {
  lora_tune_exit();                               // a profile change always ends TUNE
  if (idx < LORA_PROFILE_COUNT) lora_profile_apply(idx, remember);
  else                          lora_custom_apply(remember);
}

//=====================================================================================================
// Profile screen input (normal mode, called from ui.c handlers)
//  - jog long press: M1 TRACK (follows the SWR minimum) <-> FREE (stays where it is put);
//    short press opens the menu as usual (on release instead of on press)
//  - jog left/right: moves M1 (never the band markers), switches M1 to FREE
//  - touch: action bar buttons; only M1 can be dragged (-> FREE); the info block can be dragged
//    anywhere in the plot (session), a tap on a moved block puts it back to automatic placement,
//    a tap on its legend line cycles VIEW
//  - TUNE full screen: any tap or push exits, jog left/right does nothing
//=====================================================================================================
#define lora_box_moved()   lora_info_moved()
#define BOOT_ASK_TICKS     30                                 // x 100ms = 3 s

static void lora_full_redraw(void) {
  request_to_redraw(REDRAW_CLRSCR | REDRAW_AREA | REDRAW_FREQUENCY | REDRAW_CAL_STATUS | REDRAW_BATTERY);
}

static void lora_m1_track(bool on) {
  markers[0].enabled = TRUE;
  active_marker   = 0;
  previous_marker = MARKER_INVALID;
  if (on) props_mode|= TD_MARKER_TRACK; else props_mode&= ~TD_MARKER_TRACK;
  select_lever_mode(LM_MARKER);
  request_to_redraw(REDRAW_PLOT | REDRAW_MARKER);   // TRACK: snaps M1 to the minimum at once
}

//-----------------------------------------------------------------------------------------------------
// TUNE: narrow fast sweep (101 points over the band profile range), restored on exit
//-----------------------------------------------------------------------------------------------------
#define LORA_TUNE_POINTS 101
static struct {freq_t f0, f1; uint16_t points, mode;} lora_tune_save LORA_CCM;

// Band profile index for band b (0: 433, 1: 869, 2: 915)
static int lora_band_profile(int b) {
  for (int i = 0; i < LORA_PROFILE_COUNT; i++) if (lora_profiles[i].band == b) return i;
  return 0;
}

static bool lora_tune_enter(void) {
  int p = lora_active();
  if (p < 0 || lora_tune_active()) return false;
  int b = lora_profiles[p].band;
  if (b < 0) {                                     // WIDE/SURVEY: band nearest to the resonance
    float f = lora_res_freq(), best = 1e12f;
    for (int k = 0; k < (int)ARRAY_COUNT(lora_bands); k++) {
      float d = vna_fabsf(f - 0.5f * (float)(lora_bands[k][0] + lora_bands[k][1]));
      if (d < best) {best = d; b = k;}
    }
  }
  const lora_profile_t *bp = &lora_profiles[lora_band_profile(b)];
  lora_tune_save.f0 = frequency0; lora_tune_save.f1 = frequency1;
  lora_tune_save.points = sweep_points; lora_tune_save.mode = props_mode;
  set_sweep_points(LORA_TUNE_POINTS);
  set_sweep_frequency(ST_START, bp->start);
  set_sweep_frequency(ST_STOP,  bp->stop);
  lora_screen_t s;
  lora_profile_screen(&s);                         // cal state of the profile before the narrow sweep
  lora_tune_t t = {bp->title, bp->target, lora_bands[b][0], lora_bands[b][1], s.cal};
  lora_tune_start(&t);
  if (!(sweep_mode & SWEEP_ENABLE)) toggle_sweep();
  return true;
}

static void lora_tune_exit(void) {
  if (!lora_tune_active()) return;
  set_sweep_points(lora_tune_save.points);
  set_sweep_frequency(ST_START, lora_tune_save.f0);
  set_sweep_frequency(ST_STOP,  lora_tune_save.f1);
  props_mode = lora_tune_save.mode;
  lora_tune_stop();
}

//-----------------------------------------------------------------------------------------------------
// Profile chooser (boot row on the splash, or full screen grid from the action bar)
//-----------------------------------------------------------------------------------------------------
// Display order WIDE / SURVEY / 433 / 869 / 915 / CUSTOM -> choice index (profile index or LORA_CUSTOM)
static const uint8_t lora_disp_choice[LORA_CH_N] = {0, 4, 1, 2, 3, LORA_CUSTOM};

static int lora_choice_disp(int c) {
  for (int d = 0; d < LORA_CH_N; d++) if (lora_disp_choice[d] == c) return d;
  return LORA_CH_N - 1;
}

static void lora_choices(lora_choice_t *c) {
  for (int d = 0; d < LORA_CH_N; d++) {
    int i = lora_disp_choice[d];
    bool ok;
    if (i < LORA_PROFILE_COUNT) {
      ok = lora_has_cal(i);
      c[d].name = lora_profiles[i].shortname; c[d].range = lora_profiles[i].range;
      c[d].tag  = ok ? "cal OK" : "NO CAL";   c[d].tag_fg = ok ? LCD_NORMAL_BAT_COLOR : LCD_LOW_BAT_COLOR;
    } else {
      ok = lora_custom_valid();
      c[d].name = "CUSTOM"; c[d].range = "your own setup";
      c[d].tag  = ok ? "saved" : "empty";     c[d].tag_fg = ok ? LCD_NORMAL_BAT_COLOR : LCD_GRID_COLOR;
    }
  }
}

// Modal chooser. layout 0: boot row, auto start after 3 s unless wait; layout 1: full screen,
// waits, CANCEL possible. Returns the choice (profile index or LORA_CUSTOM), -1 = cancel
static int lora_choose(int layout, int sel, bool wait) {
  lora_choice_t c[LORA_CH_N];
  lora_choices(c);
  int d = lora_choice_disp(sel), ticks = BOOT_ASK_TICKS;
  bool timeout = layout == 0 && !wait;
  btn_check(); touch_check();                       // sync button/touch state
  lora_scr_chooser(layout, c, d, timeout ? ticks / 10 : -1, c[d].name);
  while (1) {
    uint16_t ev = btn_check();
    if (ev & EVT_BUTTON_SINGLE_CLICK) break;
    if (ev & (EVT_UP | EVT_DOWN)) {
      lora_scr_choice_button(layout, d, &c[d], false);
      d = (d + ((ev & EVT_UP) ? 1 : LORA_CH_N - 1)) % LORA_CH_N;
      lora_scr_choice_button(layout, d, &c[d], true);
      ticks = 50;
    }
    if (touch_check() == EVT_TOUCH_PRESSED) {
      int tx, ty;
      touch_position(&tx, &ty);
      touch_wait_release();
      int h = lora_scr_choice_hit(layout, tx, ty);
      if (h >= 0) {lora_scr_choice_button(layout, d, &c[d], false); d = h; lora_scr_choice_button(layout, d, &c[d], true); break;}
      if (h == -2) {d = -1; break;}
    }
    shell_poll_pending();                           // keep 'capture' working
    chThdSleepMilliseconds(100);
    if (!timeout) continue;
    if (--ticks <= 0) break;
    if (ticks % 10 == 0) lora_scr_chooser(0, c, d, ticks / 10, c[d].name);
  }
  operation_requested = OP_NONE;                    // drop events collected while choosing
  return d < 0 ? -1 : lora_disp_choice[d];
}

// 16 bit stamp of this build (git version + build time), never 0
static uint16_t lora_build_stamp(void) {
  static const char id[] = VERSION " " __DATE__ " " __TIME__;
  uint32_t h = 2166136261U;                        // FNV-1a
  for (const char *p = id; *p; p++) h = (h ^ (uint8_t)*p) * 16777619U;
  h = (h ^ (h >> 16)) & 0xFFFF;
  return h ? h : 1;
}

// First boot: no power on choice stored yet, or stored by another firmware build
static bool lora_first_boot(void) {
  int last = (lora_cfg & 0x0F) - 1;
  return last < 0 || last > LORA_CUSTOM
      || (lora_cfg_stamp_lo | (lora_cfg_stamp_hi << 8)) != lora_build_stamp();
}

static bool lora_boot_asks(void) {return !(lora_cfg & LORA_CFG_NO_ASK);}

void lora_profile_boot(void) {
  int last = (lora_cfg & 0x0F) - 1;
  bool first = lora_first_boot();
  if (last < 0 || last > LORA_CUSTOM) last = LORA_CUSTOM;   // nothing chosen yet: preselect CUSTOM
  if (first || lora_boot_asks())
    last = lora_choose(0, last, first);
  if (first) {                                              // store choice + build stamp once
    uint16_t stamp = lora_build_stamp();
    lora_cfg_stamp_lo = stamp & 0xFF;
    lora_cfg_stamp_hi = stamp >> 8;
    lora_cfg&= ~0x0F;                                       // force lora_remember() to save
  }
  lora_select(last, true);
}

// Apply a choice from the UI, hint when the profile has no calibration
static void lora_pick_apply(int c) {
  lora_select(c, true);
  if (!(sweep_mode & SWEEP_ENABLE)) toggle_sweep();
  if (c < LORA_PROFILE_COUNT && !lora_cal_ok(&lora_profiles[c])) lora_toast("NO CAL - tap CAL to calibrate");
  else if (c == LORA_CUSTOM && !lora_custom_valid()) lora_toast("CUSTOM is empty");
}

static void lora_profile_pick(void) {
  int cur = lora_active();
  int c = lora_choose(1, cur < 0 ? LORA_CUSTOM : cur, true);
  lora_full_redraw();
  if (c >= 0) lora_pick_apply(c);
}

//-----------------------------------------------------------------------------------------------------
// QUICK CAL wizard (full screen): OPEN -> SHORT -> LOAD -> save to the profile slot
//-----------------------------------------------------------------------------------------------------
// Modal wait: push or tap = 1, tap on CANCEL (if cancel) = 0, timeout (ms, 0 = none) = 1
static int lora_wait_tap(bool cancel, uint32_t timeout) {
  btn_check(); touch_check();
  for (uint32_t t = 0; ; t+= 30) {
    if (btn_check() & EVT_BUTTON_SINGLE_CLICK) return 1;
    if (touch_check() == EVT_TOUCH_PRESSED) {
      int tx, ty;
      touch_position(&tx, &ty);
      touch_wait_release();
      return (cancel && lora_scr_cal_cancel_hit(tx, ty)) ? 0 : 1;
    }
    shell_poll_pending();
    chThdSleepMilliseconds(30);
    if (timeout && t >= timeout) return 1;
  }
}

static void lora_cal_wizard(void) {
  static const uint8_t types[LORA_CAL_STEPS] = {CAL_OPEN, CAL_SHORT, CAL_LOAD};
  int p = lora_active();
  bool prof = p >= 0 && sweep_points == LORA_POINTS;
  int slot = prof ? lora_profiles[p].slot : lora_custom_slot;    // non-profile range: CUSTOM
  const char *title = prof ? lora_profiles[p].title : "CUSTOM";
  int step;
  for (step = 0; step < LORA_CAL_STEPS; step++) {
    lora_scr_cal(title, slot, step, 0, NULL);
    if (!lora_wait_tap(true, 0)) break;            // CANCEL
    lora_scr_cal(title, slot, step, 1, NULL);
    if (step == 0) {                               // start from a clean calibration (as CAL->RESET)
      cal_status&= CALSTAT_ENHANCED_RESPONSE;
      lastsaveid = NO_SAVE_SLOT;
    }
    last_button = 0;                               // as menu_calop_acb
    cal_collect(types[step]);
  }
  if (step < LORA_CAL_STEPS) {                     // cancelled: back to the saved setup
    if (step > 0) lora_select(prof ? p : LORA_CUSTOM, false);
    lora_full_redraw();
    lora_toast("CAL cancelled");
    return;
  }
  cal_done();
  bool ok = caldata_save(slot) == 0;
  if (ok && !prof) lora_remember(LORA_CUSTOM);
  lora_scr_cal(title, slot, LORA_CAL_STEPS, 2, ok ? "SAVED" : "NOT SAVED");
  lora_wait_tap(false, 3000);
  request_to_redraw(REDRAW_BACKUP);
  lora_full_redraw();
}

//-----------------------------------------------------------------------------------------------------
// Action bar: [PROFILE] [CAL] [TUNE] [REF] [HOLD] [S21]
//-----------------------------------------------------------------------------------------------------
void lora_draw_bar(void) {
  int p = lora_active();
  lora_screen_t s;
  bool on = lora_profile_screen(&s);
  pixel_t norm = GET_PALETTE_COLOR(LCD_MENU_COLOR), off = GET_PALETTE_COLOR(LCD_GRID_COLOR);
  lora_scr_bar_button(0, "PROFILE", p >= 0 ? lora_profiles[p].shortname : "-", norm, LCD_MENU_TEXT_COLOR);
  if (on && s.cal == LORA_CAL_OK)
    lora_scr_bar_button(1, "CAL", "OK", norm, LCD_MENU_TEXT_COLOR);
  else if (on && s.cal == LORA_CAL_RANGE)
    lora_scr_bar_button(1, "CAL!", "other range", GET_PALETTE_COLOR(LCD_TRACE_1_COLOR), LCD_MENU_TEXT_COLOR);
  else
    lora_scr_bar_button(1, "CAL!", "none", GET_PALETTE_COLOR(LCD_LOW_BAT_COLOR), LCD_FG_COLOR);
  lora_scr_bar_button(2, "TUNE", "big digits", norm, LCD_MENU_TEXT_COLOR);
  if (lora_ref_valid())
    lora_scr_bar_button(3, "REF *", "hold: clear", GET_PALETTE_COLOR(LCD_TRACE_2_COLOR), LCD_MENU_TEXT_COLOR);
  else
    lora_scr_bar_button(3, "REF", "tap: store", norm, LCD_MENU_TEXT_COLOR);
  lora_scr_bar_button(4, "HOLD", "soon", off, LCD_FG_COLOR);
  lora_scr_bar_button(5, "S21", "soon", off, LCD_FG_COLOR);
}

static bool lora_touch_bar(int tx, int ty) {
  if (ty < LORA_BAR_Y) return false;
  int b = tx / LORA_BAR_W;
  systime_t t0 = chVTGetSystemTimeX();
  touch_wait_release();
  bool hold = chVTTimeElapsedSinceX(t0) > MS2ST(700);
  switch (b) {
    case 0: lora_profile_pick(); break;
    case 1: lora_cal_wizard(); break;
    case 2: lora_tune_enter(); break;
    case 3:
      if (hold) {
        if (lora_ref_valid()) {lora_ref_clear(); lora_toast("REF cleared");}
      } else {
        bool had = lora_ref_valid();
        if (lora_ref_store()) lora_toast(had ? "REF updated (hold REF to clear)" : "REF saved (hold REF to clear)");
      }
      break;
    default: lora_toast(b == 4 ? "HOLD: coming in the next update" : "S21: coming in the next update"); break;
  }
  request_to_redraw(REDRAW_FREQUENCY);
  return true;
}

//-----------------------------------------------------------------------------------------------------
// ui.c hooks
//-----------------------------------------------------------------------------------------------------
// Jog left/right: true = consumed
static bool lora_jog_marker(void) {
  if (lora_tune_active()) return true;
  if (!lora_screen_active()) return false;
  if (active_marker != 0 || (props_mode & TD_MARKER_TRACK)) lora_m1_track(false);
  return false;
}

// Jog push: true = consumed (TUNE: exit; profile screen long press: M1 TRACK <-> FREE)
static bool lora_jog_push(void) {
  if (lora_tune_active()) {lora_tune_exit(); return true;}
  if (!lora_screen_active()) return false;
  if (btn_wait_release() != EVT_BUTTON_DOWN_LONG) return false;   // short press: menu
  lora_m1_track(!(props_mode & TD_MARKER_TRACK));
  lora_toast((props_mode & TD_MARKER_TRACK) ? "M1 TRACK: follows the SWR minimum" : "M1 FREE: jog moves M1");
  while (get_buttons() & BUTTON_PUSH) chThdSleepMilliseconds(10);  // ignore the release
  last_button = get_buttons();
  return true;
}

static bool lora_touch_info(int tx, int ty) {
  int bx, by, bw, bh;
  if (tx <= UI_SCALE_REF_X1 || !lora_info_rect(&bx, &by, &bw, &bh)) return false;  // keep scale/ref area
  int ax = tx - OFFSETX, ay = ty - OFFSETY;
  if (ax < bx || ax >= bx + bw || ay < by || ay >= by + bh) return false;
  bool moved = false;
  do {
    int x, y;
    touch_position(&x, &y);
    int dx = x - tx, dy = y - ty;
    if (!moved && dx * dx + dy * dy > 8 * 8) moved = true;
    if (moved) {
      lora_info_place(bx + dx, by + dy);
      request_to_redraw(REDRAW_CELLS);
      draw_all();
    } else
      chThdSleepMilliseconds(20);
  } while (touch_check() != EVT_TOUCH_RELEASED);
  if (moved)
    ;                                 // position kept for this session
  else if (ay >= by + bh - STR_MEASURE_HEIGHT - 4) {   // legend line: next VIEW
    lora_view_set((lora_view() + 1) % LORA_VIEW_COUNT);
    char msg[24];
    plot_printf(msg, sizeof(msg), "VIEW: %s", lora_view_names[lora_view()]);
    lora_toast(msg);
  } else if (lora_box_moved()) {      // tap on a moved block: automatic placement again
    lora_info_place(-1, 0);
    request_to_redraw(REDRAW_CELLS);
    lora_toast("Info block: auto position");
  } else
    ui_mode_menu();                   // tap on the block: same as a tap elsewhere on the plot
  return true;
}

// Normal mode touch: TUNE exit, action bar, info block
static bool lora_touch(int tx, int ty) {
  if (lora_tune_active()) {touch_wait_release(); lora_tune_exit(); return true;}
  if (!lora_screen_active()) return false;
  return lora_touch_bar(tx, ty) || lora_touch_info(tx, ty);
}

//=====================================================================================================
// Profiles menu
//=====================================================================================================
static UI_FUNCTION_ADV_CALLBACK(menu_lora_profile_acb) {
  const lora_profile_t *p = &lora_profiles[data];
  if (b) {
    bool active = lora_active() == (int)data;
    bool cal = lora_has_cal(data) || (active && lora_cal_ok(p));
    b->icon = active ? BUTTON_ICON_GROUP_CHECKED : BUTTON_ICON_GROUP;
    if (cal)
      plot_printf(b->label, sizeof(b->label), "%s\nCAL #%d", p->label, lora_slot(data));
    else {
      plot_printf(b->label, sizeof(b->label), "%s\nNO CAL", p->label);
      b->fg = LCD_LOW_BAT_COLOR;  // red text: not calibrated
    }
    return;
  }
  ui_mode_normal();
  lora_pick_apply(data);          // uncalibrated: hint to tap CAL
}

static UI_FUNCTION_CALLBACK(menu_lora_qcal_cb) {
  (void)data;
  ui_mode_normal();
  lora_cal_wizard();
}

static UI_FUNCTION_ADV_CALLBACK(menu_lora_custom_acb) {
  (void)data;
  bool ok = lora_custom_valid();
  if (b) {
    b->icon = (lora_active() < 0 && (lora_cfg & 0x0F) == LORA_CUSTOM + 1) ? BUTTON_ICON_GROUP_CHECKED : BUTTON_ICON_GROUP;
    plot_printf(b->label, sizeof(b->label), "CUSTOM\nslot #%d\n%s", lora_custom_slot, ok ? "saved" : "empty");
    return;
  }
  if (!ok) {
    ui_message_box("CUSTOM is empty", "Use OPTIONS > SAVE", 2000);
    return;
  }
  lora_custom_apply(true);
  if (!(sweep_mode & SWEEP_ENABLE)) toggle_sweep();
  ui_mode_normal();
}

static UI_FUNCTION_ADV_CALLBACK(menu_lora_autoscale_acb) {
  (void)data;
  if (b) {
    b->icon = lora_autoscale_mask ? BUTTON_ICON_CHECK : BUTTON_ICON_NOCHECK;
    return;
  }
  lora_autoscale_on = lora_autoscale_mask == 0;
  lora_autoscale_mask = lora_autoscale_on ? 0x0F : 0;
  request_to_redraw(REDRAW_PLOT);
}

static UI_FUNCTION_ADV_CALLBACK(menu_lora_view_acb) {
  (void)data;
  if (b) {
    plot_printf(b->label, sizeof(b->label), "VIEW\n%s", lora_view_names[lora_view()]);
    return;
  }
  lora_view_set((lora_view() + 1) % LORA_VIEW_COUNT);
}

static UI_FUNCTION_ADV_CALLBACK(menu_lora_ask_acb) {
  (void)data;
  if (b) {
    b->icon = lora_boot_asks() ? BUTTON_ICON_CHECK : BUTTON_ICON_NOCHECK;
    return;
  }
  lora_cfg ^= LORA_CFG_NO_ASK;
  config_save();
}

static UI_FUNCTION_CALLBACK(menu_lora_save_custom_cb) {
  (void)data;
  char msg[24];
  if (caldata_save(lora_custom_slot) == 0) {
    lora_remember(LORA_CUSTOM);
    plot_printf(msg, sizeof(msg), "Saved to slot %d", lora_custom_slot);
  } else
    plot_printf(msg, sizeof(msg), "Save failed");
  request_to_redraw(REDRAW_BACKUP | REDRAW_CAL_STATUS);
  menu_move_back(true);
  ui_message_box("CUSTOM", msg, 1500);
}

static UI_FUNCTION_ADV_CALLBACK(menu_lora_box_auto_acb) {
  (void)data;
  if (b) {
    b->icon = lora_box_moved() ? BUTTON_ICON_NOCHECK : BUTTON_ICON_CHECK;
    return;
  }
  if (!lora_box_moved()) return;
  lora_info_place(-1, 0);
  request_to_redraw(REDRAW_AREA);
}

static const menuitem_t menu_lora_options[] = {
  { MT_ADV_CALLBACK, 0, "AUTOSCALE",           menu_lora_autoscale_acb },
  { MT_ADV_CALLBACK, 0, "INFO BOX\nAUTO POS",  menu_lora_box_auto_acb },
  { MT_ADV_CALLBACK, 0, "ASK AT\nPOWER ON",    menu_lora_ask_acb },
  { MT_CALLBACK,     0, "SAVE AS\nCUSTOM",     menu_lora_save_custom_cb },
  { MT_NEXT, 0, NULL, menu_back } // next-> menu_back
};

static const menuitem_t menu_profiles[] = {
  { MT_ADV_CALLBACK, 0, "WIDE", menu_lora_profile_acb },
  { MT_ADV_CALLBACK, 1, "433",  menu_lora_profile_acb },
  { MT_ADV_CALLBACK, 2, "869",  menu_lora_profile_acb },
  { MT_ADV_CALLBACK, 3, "915",  menu_lora_profile_acb },
  { MT_ADV_CALLBACK, 4, "SURVEY", menu_lora_profile_acb },
  { MT_ADV_CALLBACK, 0, "CUSTOM", menu_lora_custom_acb },
  { MT_CALLBACK,     0, "QUICK CAL", menu_lora_qcal_cb },
  { MT_ADV_CALLBACK, 0, "VIEW",      menu_lora_view_acb },
  { MT_SUBMENU,      0, "OPTIONS", menu_lora_options },
  { MT_NEXT, 0, NULL, menu_back } // next-> menu_back
};

//=====================================================================================================
// Shell command: profile [wide|433|869|915|survey|custom] | profile view [rl|smith|all]
//                profile info | profile ref [store|clear] | profile tune [on|off]
//=====================================================================================================
int shell_printf(const char *fmt, ...);
static void lora_profile_print(int i) {
  const lora_profile_t *p = &lora_profiles[i];
  shell_printf("%s %u %u %d %d %d %d\r\n", p->name, p->start, p->stop, LORA_POINTS, lora_slot(i),
               lora_has_cal(i) ? 1 : 0, lora_active() == i ? 1 : 0);
}

static void lora_custom_print(void) {
  const properties_t *sp = get_properties(lora_custom_slot);
  shell_printf("custom %u %u %d %d %d %d\r\n", sp ? sp->_frequency0 : 0, sp ? sp->_frequency1 : 0,
               sp ? sp->_sweep_points : 0, lora_custom_slot, sp ? 1 : 0,
               (lora_active() < 0 && (lora_cfg & 0x0F) == LORA_CUSTOM + 1) ? 1 : 0);
}

void cmd_profile(int argc, char *argv[]) {
  int i;
  if (argc == 0) {
    for (i = 0; i < LORA_PROFILE_COUNT; i++)
      lora_profile_print(i);
    lora_custom_print();
    i = (lora_cfg & 0x0F) - 1;
    shell_printf("boot %s ask=%d\r\n", (i >= 0 && i < LORA_PROFILE_COUNT) ? lora_profiles[i].name : "custom",
                 lora_boot_asks() ? 1 : 0);
    shell_printf("view %s\r\n", lora_view_names[lora_view()]);
    return;
  }
  if (get_str_index(argv[0], "view") == 0) {
    if (argc == 2 && (i = get_str_index(argv[1], LORA_VIEW_SHELL)) >= 0)
      lora_view_set(i);
    else if (argc != 1) goto usage;
    shell_printf("view %s\r\n", lora_view_names[lora_view()]);
    return;
  }
  if (get_str_index(argv[0], "info") == 0) {lora_info_print(LORA_PRINT_INFO | LORA_PRINT_REF | LORA_PRINT_TUNE); return;}
  if (get_str_index(argv[0], "ref") == 0) {
    if (argc == 2) {
      i = get_str_index(argv[1], "store|clear");
      if (i < 0) goto usage;
      if (i == 1) lora_ref_clear();
      else if (!lora_ref_store()) {shell_printf("error: not on a profile screen\r\n"); return;}
    }
    lora_info_print(LORA_PRINT_REF);
    return;
  }
  if (get_str_index(argv[0], "tune") == 0) {
    if (argc == 2) {
      i = get_str_index(argv[1], "on|off");
      if (i < 0) goto usage;
      if (i == 1) lora_tune_exit();
      else if (!lora_tune_active() && !lora_tune_enter()) {shell_printf("error: not on a profile\r\n"); return;}
    }
    lora_info_print(LORA_PRINT_TUNE);
    return;
  }
  if (argc == 1 && (i = get_str_index(argv[0], LORA_PROFILE_NAMES "|custom")) >= 0) {
    lora_select(i, true);
    if (i < LORA_PROFILE_COUNT) lora_profile_print(i); else lora_custom_print();
    return;
  }
usage:
  shell_printf("usage: profile [" LORA_PROFILE_NAMES "|custom] | profile view [" LORA_VIEW_SHELL "]\r\n"
               "       profile info | profile ref [store|clear] | profile tune [on|off]\r\n");
}
#endif // __VNA_LORA_PROFILES__

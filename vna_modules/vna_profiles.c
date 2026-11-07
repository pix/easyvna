/*
 * Quick antenna measurement profiles + quick S11 calibration (LoRa bands)
 *
 * This file is included from ui.c (like the other vna_modules), so it can use
 * the static menu framework of ui.c. Enabled by __VNA_LORA_PROFILES__.
 * Plot side (autoscale, LORA INFO text) is in vna_lora_plot.c (included from plot.c).
 *
 * PROFILES menu:
 *   - each profile sets sweep range + points, an S11-only display (SWR bold + RL, or
 *     SWR + Smith, or all of SWR/RL/X/Smith: VIEW), autoscaled, the LORA INFO block,
 *     marker 1 tracking the SWR minimum and markers on the ISM band edges (WIDE: on the
 *     433/868/915 band centres). Everything else (other markers, delta/reference marker,
 *     stored trace, lever mode) is reset.
 *   - each profile owns a fixed save slot (the last 4 slots); if that slot holds a
 *     calibration made over exactly the profile range/points it is recalled first,
 *     otherwise QUICK CAL is opened (from the menu).
 *   - CUSTOM: the user's own setup (any range/display/cal) stored in one more slot
 *     (OPTIONS > SAVE AS CUSTOM, or QUICK CAL on a non-profile range).
 *   - last used profile/custom is stored in config. At power on a chooser on the
 *     splash screen preselects it and starts it after 3 s (jog/touch to pick another);
 *     OPTIONS > ASK AT POWER ON off = start the last one without asking.
 *     First boot (nothing chosen yet, or first boot of a newly flashed firmware: build
 *     stamp in config differs) the chooser is always shown and waits for a choice.
 * QUICK CAL menu:
 *   - OPEN -> SHORT -> LOAD -> DONE; DONE saves to the slot of the profile whose
 *     range matches the calibration range (or keeps the cal in RAM only).
 * Shell:
 *   - "profile"              list: name start stop points slot cal(0/1) active(0/1),
 *                            then "boot <name> ask=<0|1>"
 *   - "profile <name>"       apply profile (wide|433|869|915|custom), prints its line
 *   - "profile view [rl|smith|all]"  show / set the profile VIEW (SWR+RL, SWR+Smith, all)
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
  freq_t start, stop;   // sweep range
  int8_t band;          // ISM band in lora_bands[] (shading, edge markers, pass/fail), -1 = all (WIDE)
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
#define LORA_PROFILE_NAMES "wide|433|869|915"
// Band profiles: centre +-~10% (dip of a detuned antenna stays in view), 401 points
// (225..450 kHz step, 8..58 points inside the band). WIDE: find a far detuned antenna.
static const lora_profile_t lora_profiles[] = {
  {"wide", "WIDE 300M-1G", "WIDE\n300M-1G",   300000000U, 1000000000U, -1,          0},
  {"433",  "433 ISM",      "433 ISM\n389-479M", 388920000U,  478920000U,  0, 433920000U},
  {"869",  "869 EU",       "869 EU\n779-959M",  779000000U,  959000000U,  1, 869000000U},
  {"915",  "915 US",       "915 US\n825-1005M", 825000000U, 1005000000U,  2, 915000000U},
};
#define LORA_PROFILE_COUNT ((int)ARRAY_COUNT(lora_profiles))
#define LORA_POINTS        POINTS_COUNT_DEFAULT
// Profiles use the last save slots (H4: 3..6), CUSTOM the one before (H4: 2),
// slot 0 (stock power-on slot) stays free on H4
#define LORA_CUSTOM        LORA_PROFILE_COUNT   // index used for CUSTOM
#define lora_slot(i)       (SAVEAREA_MAX - LORA_PROFILE_COUNT + (i))
#define lora_custom_slot   lora_slot(-1)
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

#if SAVEAREA_MAX < 5
#error "LoRa profiles need at least 5 save slots"
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

static void lora_select(int idx, bool remember) {
  if (idx < LORA_PROFILE_COUNT) lora_profile_apply(idx, remember);
  else                          lora_custom_apply(remember);
}

//=====================================================================================================
// Profile screen input (normal mode, called from ui.c handlers)
//  - jog long press: M1 TRACK (follows the SWR minimum) <-> FREE (stays where it is put);
//    short press opens the menu as usual (on release instead of on press)
//  - jog left/right: moves M1 (never the band markers), switches M1 to FREE
//  - touch: only M1 can be dragged (-> FREE); the info block can be dragged anywhere in the plot
//    (position saved once on release), a tap on a moved block puts it back to automatic placement
//=====================================================================================================
#define lora_box_moved()   lora_info_moved()

static void lora_m1_track(bool on) {
  markers[0].enabled = TRUE;
  active_marker   = 0;
  previous_marker = MARKER_INVALID;
  if (on) props_mode|= TD_MARKER_TRACK; else props_mode&= ~TD_MARKER_TRACK;
  select_lever_mode(LM_MARKER);
  request_to_redraw(REDRAW_PLOT | REDRAW_MARKER);   // TRACK: snaps M1 to the minimum at once
}

static void lora_jog_marker(void) {
  if (!lora_screen_active()) return;
  if (active_marker != 0 || (props_mode & TD_MARKER_TRACK)) lora_m1_track(false);
}

static bool lora_jog_push(void) {
  if (!lora_screen_active()) return false;
  if (btn_wait_release() != EVT_BUTTON_DOWN_LONG) return false;   // short press: menu
  lora_m1_track(!(props_mode & TD_MARKER_TRACK));
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
  else if (lora_box_moved()) {        // tap on a moved block: automatic placement again
    lora_info_place(-1, 0);
    request_to_redraw(REDRAW_CELLS);
  } else
    ui_mode_menu();                   // tap on the block: same as a tap elsewhere on the plot
  return true;
}

//=====================================================================================================
// Power on chooser, drawn over the splash screen (called in sweep thread after ui_init())
//=====================================================================================================
#define BOOT_ASK_TICKS   30                                   // x 100ms = 3 s
#define BOOT_BTN_H       (FONT_STR_HEIGHT * 2 + 10)
#define BOOT_BTN_GAP     6
#define BOOT_BTN_Y       (LCD_HEIGHT - 6 - 2 * (FONT_STR_HEIGHT + 2) - 6 - BOOT_BTN_H)
#define BOOT_TEXT_Y      (BOOT_BTN_Y - FONT_STR_HEIGHT - 6)    // top of chooser area (splash keeps above)
#define BOOT_CHOICES     (LORA_PROFILE_COUNT + 1)
#define BOOT_BTN_W       ((LCD_WIDTH - 2 * 8 - (BOOT_CHOICES - 1) * BOOT_BTN_GAP) / BOOT_CHOICES)

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

// secs < 0: no countdown (first boot, wait for a choice)
static void lora_boot_draw(int sel, int secs) {
  static const char *names[BOOT_CHOICES] = {"WIDE", "433", "869", "915", "CUSTOM"};
  for (int i = 0; i < BOOT_CHOICES; i++) {
    button_t b;
    int x = 8 + i * (BOOT_BTN_W + BOOT_BTN_GAP);
    b.bg = (i == sel) ? LCD_TRACE_1_COLOR : LCD_MENU_COLOR;
    b.fg = LCD_MENU_TEXT_COLOR;
    b.border = MENU_BUTTON_BORDER | ((i == sel) ? BUTTON_BORDER_FALLING : BUTTON_BORDER_RISE);
    ui_draw_button(x, BOOT_BTN_Y, BOOT_BTN_W, BOOT_BTN_H, &b);
    bool ok = (i < LORA_PROFILE_COUNT) ? lora_has_cal(i) : lora_custom_valid();
    const char *l2 = (i < LORA_PROFILE_COUNT) ? (ok ? "cal" : "NO CAL") : (ok ? "saved" : "stock");
    lcd_drawstring(x + (BOOT_BTN_W - (int)strlen(names[i]) * FONT_WIDTH) / 2, BOOT_BTN_Y + 4, names[i]);
    if (!ok && i < LORA_PROFILE_COUNT) lcd_set_foreground(LCD_LOW_BAT_COLOR);
    lcd_drawstring(x + (BOOT_BTN_W - (int)strlen(l2) * FONT_WIDTH) / 2, BOOT_BTN_Y + 6 + FONT_STR_HEIGHT, l2);
  }
  lcd_set_colors(LCD_FG_COLOR, LCD_BG_COLOR);
  lcd_fill(0, BOOT_TEXT_Y, LCD_WIDTH, FONT_STR_HEIGHT);
  if (secs < 0)
    lcd_printf(8, BOOT_TEXT_Y, "First start: choose a mode (jog + push, or touch)");
  else
    lcd_printf(8, BOOT_TEXT_Y, "Starting %s in %ds - jog/touch to choose", names[sel], secs);
}

// Never blocks forever: auto start after 3 s, jog movement restarts a 5 s countdown,
// jog push or touch on a button starts at once (touch elsewhere is ignored)
static int lora_boot_ask(int sel, bool wait) {
  int ticks = BOOT_ASK_TICKS;
  btn_check(); touch_check();                       // sync button/touch state
  lora_boot_draw(sel, wait ? -1 : ticks / 10);
  while (ticks > 0) {
    uint16_t ev = btn_check();
    if (ev & EVT_BUTTON_SINGLE_CLICK) break;
    if (ev & (EVT_UP | EVT_DOWN)) {
      sel = (sel + ((ev & EVT_UP) ? 1 : BOOT_CHOICES - 1)) % BOOT_CHOICES;
      ticks = 50;
      lora_boot_draw(sel, wait ? -1 : ticks / 10);
    }
    if (touch_check() == EVT_TOUCH_PRESSED) {
      int tx, ty;
      touch_position(&tx, &ty);
      touch_wait_release();
      int i = (tx - 8) / (BOOT_BTN_W + BOOT_BTN_GAP);
      if (ty >= BOOT_BTN_Y && ty < BOOT_BTN_Y + BOOT_BTN_H && tx >= 8 && i < BOOT_CHOICES) {
        sel = i;
        lora_boot_draw(sel, wait ? -1 : 0);
        break;
      }
    }
    chThdSleepMilliseconds(100);
    if (wait) continue;                             // first boot: no timeout
    if (--ticks % 10 == 0) lora_boot_draw(sel, ticks / 10);
  }
  operation_requested = OP_NONE;                    // drop events collected while choosing
  return sel;
}

static bool lora_boot_asks(void) {return !(lora_cfg & LORA_CFG_NO_ASK);}

void lora_profile_boot(void) {
  int last = (lora_cfg & 0x0F) - 1;
  bool first = lora_first_boot();
  if (last < 0 || last > LORA_CUSTOM) last = LORA_CUSTOM;   // nothing chosen yet: preselect CUSTOM
  if (first || lora_boot_asks())
    last = lora_boot_ask(last, first);
  if (first) {                                              // store choice + build stamp once
    uint16_t stamp = lora_build_stamp();
    lora_cfg_stamp_lo = stamp & 0xFF;
    lora_cfg_stamp_hi = stamp >> 8;
    lora_cfg&= ~0x0F;                                       // force lora_remember() to save
  }
  lora_select(last, true);
}

//=====================================================================================================
// Quick calibration menu
//=====================================================================================================
static const struct {
  uint8_t  type;
  uint16_t mask;
  const char *name;
} qcal_list[] = {
  {CAL_OPEN,  CALSTAT_OPEN,  "1 OPEN"},
  {CAL_SHORT, CALSTAT_SHORT, "2 SHORT"},
  {CAL_LOAD,  CALSTAT_LOAD,  "3 LOAD"},
};
// Steps collected in this quick cal session
static uint8_t qcal_steps = 0;

static bool qcal_step_done(int i) {
  return (qcal_steps & (1<<i)) && (cal_status & qcal_list[i].mask);
}

static bool qcal_ready(void) {
  return qcal_step_done(0) && qcal_step_done(1) && qcal_step_done(2);
}

// Profile index for the collected calibration (range + points), -1 if none
static int qcal_profile(void) {
  if (cal_sweep_points != LORA_POINTS) return -1;
  for (int i = 0; i < LORA_PROFILE_COUNT; i++)
    if (cal_frequency0 == lora_profiles[i].start && cal_frequency1 == lora_profiles[i].stop) return i;
  return -1;
}

static UI_FUNCTION_ADV_CALLBACK(menu_qcal_step_acb) {
  if (b) {
    bool done = qcal_step_done(data);
    b->icon = done ? BUTTON_ICON_CHECK : BUTTON_ICON_NOCHECK;
    plot_printf(b->label, sizeof(b->label), "%s\n %s", qcal_list[data].name, done ? "done" : "connect");
    return;
  }
  if (qcal_steps == 0) {          // First step: start from a clean calibration (as CAL->RESET)
    cal_status &= CALSTAT_ENHANCED_RESPONSE;
    lastsaveid = NO_SAVE_SLOT;
  }
  // TODO: Hack! reset button state (same as menu_calop_acb)
  last_button = 0;
  cal_collect(qcal_list[data].type);
  qcal_steps |= 1<<data;
  selection = data + 1;           // jog wheel: move to next step
}

static UI_FUNCTION_ADV_CALLBACK(menu_qcal_done_acb) {
  (void)data;
  int p = qcal_profile();
  int slot = p >= 0 ? lora_slot(p) : lora_custom_slot;  // non-profile range: save as CUSTOM
  if (b) {
    if (!qcal_ready())
      b->p1.text = "need O/S/L";
    else
      plot_printf(b->label, sizeof(b->label), p >= 0 ? "4 DONE\n SAVE #%d" : "4 DONE\n CUSTOM #%d", slot);
    return;
  }
  if (!qcal_ready()) return;
  cal_done();
  qcal_steps = 0;
  char msg[24];
  if (caldata_save(slot) == 0) {
    plot_printf(msg, sizeof(msg), "Saved to slot %d", slot);
    if (p < 0) lora_remember(LORA_CUSTOM);
  } else
    plot_printf(msg, sizeof(msg), "Applied, not saved");
  request_to_redraw(REDRAW_BACKUP | REDRAW_CAL_STATUS | REDRAW_AREA);
  menu_move_back(true);
  ui_message_box("QUICK CAL", msg, 1500);
}

extern const menuitem_t menu_cal[];
static const menuitem_t menu_qcal[] = {
  { MT_ADV_CALLBACK, 0, "OPEN",        menu_qcal_step_acb },
  { MT_ADV_CALLBACK, 1, "SHORT",       menu_qcal_step_acb },
  { MT_ADV_CALLBACK, 2, "LOAD",        menu_qcal_step_acb },
  { MT_ADV_CALLBACK, 0, "4 DONE\n %s", menu_qcal_done_acb },
  { MT_SUBMENU,      0, "FULL CAL\n ISOLN/THRU", menu_cal },
  { MT_NEXT, 0, NULL, menu_back } // next-> menu_back
};

static void qcal_enter(void) {
  qcal_steps = 0;
  menu_push_submenu(menu_qcal);
  selection = 0;                  // OPEN highlighted
}

static UI_FUNCTION_CALLBACK(menu_qcal_cb) {
  (void)data;
  qcal_enter();
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
  lora_profile_apply(data, true);
  if (!(sweep_mode & SWEEP_ENABLE)) toggle_sweep();
  if (lora_cal_ok(p))
    ui_mode_normal();
  else
    qcal_enter();                 // uncalibrated: go straight to QUICK CAL
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
  { MT_ADV_CALLBACK, 0, "CUSTOM", menu_lora_custom_acb },
  { MT_CALLBACK,     0, "QUICK CAL", menu_qcal_cb },
  { MT_ADV_CALLBACK, 0, "VIEW",      menu_lora_view_acb },
  { MT_SUBMENU,      0, "OPTIONS", menu_lora_options },
  { MT_NEXT, 0, NULL, menu_back } // next-> menu_back
};

//=====================================================================================================
// Shell command: profile [wide|433|869|915|custom] | profile view [rl|smith|all]
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
  if (argc == 1 && (i = get_str_index(argv[0], LORA_PROFILE_NAMES "|custom")) >= 0) {
    lora_select(i, true);
    if (i < LORA_PROFILE_COUNT) lora_profile_print(i); else lora_custom_print();
    return;
  }
usage:
  shell_printf("usage: profile [" LORA_PROFILE_NAMES "|custom] | profile view [" LORA_VIEW_SHELL "]\r\n");
}
#endif // __VNA_LORA_PROFILES__

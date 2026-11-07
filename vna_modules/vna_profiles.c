/*
 * Quick antenna measurement profiles + quick S11 calibration (LoRa bands)
 *
 * This file is included from ui.c (like the other vna_modules), so it can use
 * the static menu framework of ui.c. Enabled by __VNA_LORA_PROFILES__.
 *
 * PROFILES menu:
 *   - each profile sets a sweep range, an S11-only display (SWR + Smith on CH0),
 *     marker 1 tracking the SWR minimum and (for band profiles) markers 2/3 on
 *     the band edges.
 *   - each profile owns a fixed save slot; if that slot holds a calibration made
 *     over exactly the profile range it is recalled first.
 * QUICK CAL menu:
 *   - OPEN -> SHORT -> LOAD -> DONE; DONE saves to the slot of the profile whose
 *     range matches the calibration range (or keeps the cal in RAM only).
 * Shell:
 *   - "profile"         list profiles: name start stop slot cal(0/1) active(0/1)
 *   - "profile <name>"  apply profile, prints the same line for it
 *
 * This is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3, or (at your option) any later version.
 */
#ifdef __VNA_LORA_PROFILES__

typedef struct {
  const char *name;     // shell name
  const char *label;    // menu label (name + range)
  freq_t start, stop;   // sweep range
  freq_t band_lo;       // band edge markers (0 = none)
  freq_t band_hi;
  uint8_t slot;         // save slot used for this profile calibration
} lora_profile_t;

// Shell names list, must be in the same order as lora_profiles[]
#define LORA_PROFILE_NAMES "wide|433|869|915"
static const lora_profile_t lora_profiles[] = {
  {"wide", "WIDE\n 300M-1G",    300000000U, 1000000000U,         0,          0, 0},
  {"433",  "433 ISM\n 384-484M", 384000000U,  484000000U, 433050000U, 434790000U, 1},
  {"869",  "869 EU\n 819-919M",  819000000U,  919000000U, 863000000U, 870000000U, 2},
  {"915",  "915 US\n 865-965M",  865000000U,  965000000U, 902000000U, 928000000U, 3},
};
#define LORA_PROFILE_COUNT ARRAY_COUNT(lora_profiles)

#if SAVEAREA_MAX < 4
#error "LoRa profiles need at least 4 save slots"
#endif

static bool lora_profile_active(const lora_profile_t *p) {
  return frequency0 == p->start && frequency1 == p->stop;
}

// Slot holds a calibration collected over exactly the profile range
static bool lora_profile_has_cal(const lora_profile_t *p) {
  const properties_t *sp = get_properties(p->slot);
  return sp && (sp->_cal_status & (CALSTAT_OPEN|CALSTAT_SHORT|CALSTAT_LOAD))
            && sp->_cal_frequency0 == p->start && sp->_cal_frequency1 == p->stop;
}

// Return profile matching range or -1
static int lora_profile_by_range(freq_t start, freq_t stop) {
  for (int i = 0; i < (int)LORA_PROFILE_COUNT; i++)
    if (lora_profiles[i].start == start && lora_profiles[i].stop == stop) return i;
  return -1;
}

static void lora_marker_at(int m, freq_t f) {
  markers[m].enabled = TRUE;
  int idx = (float)(f - frequency0) * (sweep_points - 1) / (float)(frequency1 - frequency0) + 0.5f;
  set_marker_index(m, idx);
}

// Apply profile, return true if a calibration for this profile was recalled
static bool lora_profile_apply(int idx) {
  const lora_profile_t *p = &lora_profiles[idx];
  // Recall slot calibration (restores sweep range/points/cal) if it matches
  bool recalled = lora_profile_has_cal(p) && load_properties(p->slot) == 0;
  // Sweep range
  set_sweep_frequency(ST_START, p->start);
  set_sweep_frequency(ST_STOP,  p->stop);
  if (p->band_lo) FREQ_CENTERSPAN();  // show "CENTER xxx SPAN 100M" for band profiles
  // Frequency domain, no delta markers, marker tracking on
  props_mode = (props_mode & ~(DOMAIN_MODE|TD_MARKER_DELTA)) | TD_MARKER_TRACK;
#ifdef __VNA_MEASURE_MODULE__
  plot_set_measure_mode(MEASURE_NONE);
#endif
  // S11 only display: SWR (1..5) + Smith on CH0, all other traces off
  static const trace_t lora_trace[2] = {
    { TRUE, TRC_SWR,   0, MS_RX, 0.5f, 0 },
    { TRUE, TRC_SMITH, 0, MS_RX, 1.0f, 0 },
  };
  memcpy(&trace[0], lora_trace, sizeof(lora_trace));
  for (int t = 2; t < TRACES_MAX; t++)
    trace[t].enabled = FALSE;
  current_trace = 0;
  // Markers: M1 = SWR minimum (tracking), M2/M3 = band edges
  for (int m = 0; m < MARKERS_MAX; m++)
    markers[m].enabled = FALSE;
  markers[0].enabled = TRUE;
  active_marker   = 0;
  previous_marker = MARKER_INVALID;
  if (p->band_lo) {
    lora_marker_at(1, p->band_lo);
    lora_marker_at(2, p->band_hi);
    previous_marker = 1;
  }
  // Marker search = minimum (global config option, not saved until SAVE CONFIG)
  apply_VNA_mode(VNA_MODE_SEARCH, VNA_MODE_SET);
  // No REDRAW_CLRSCR: may be called from shell while the menu is open
  request_to_redraw(REDRAW_AREA | REDRAW_PLOT | REDRAW_MARKER | REDRAW_FREQUENCY | REDRAW_CAL_STATUS | REDRAW_BACKUP);
  return recalled;
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
  int p = lora_profile_by_range(cal_frequency0, cal_frequency1);
  if (b) {
    if (!qcal_ready())
      b->p1.text = "need O/S/L";
    else if (p >= 0)
      plot_printf(b->label, sizeof(b->label), "4 DONE\n SAVE #%d", lora_profiles[p].slot);
    else
      b->p1.text = "RAM only";
    return;
  }
  if (!qcal_ready()) return;
  cal_done();
  qcal_steps = 0;
  char msg[24];
  if (p >= 0 && caldata_save(lora_profiles[p].slot) == 0)
    plot_printf(msg, sizeof(msg), "Saved to slot %d", lora_profiles[p].slot);
  else
    plot_printf(msg, sizeof(msg), "Applied, not saved");
  request_to_redraw(REDRAW_BACKUP | REDRAW_CAL_STATUS);
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

static UI_FUNCTION_CALLBACK(menu_qcal_cb) {
  (void)data;
  qcal_steps = 0;
  menu_push_submenu(menu_qcal);
  selection = 0;
}

//=====================================================================================================
// Profiles menu
//=====================================================================================================
static UI_FUNCTION_ADV_CALLBACK(menu_lora_profile_acb) {
  const lora_profile_t *p = &lora_profiles[data];
  if (b) {
    b->icon = lora_profile_active(p) ? BUTTON_ICON_GROUP_CHECKED : BUTTON_ICON_GROUP;
    plot_printf(b->label, sizeof(b->label), lora_profile_has_cal(p) ? "%s\n CAL #%d" : "%s\n no cal", p->label, p->slot);
    return;
  }
  lora_profile_apply(data);
  if (!(sweep_mode & SWEEP_ENABLE)) toggle_sweep();
  ui_mode_normal();
}

static const menuitem_t menu_profiles[] = {
  { MT_ADV_CALLBACK, 0, "WIDE", menu_lora_profile_acb },
  { MT_ADV_CALLBACK, 1, "433",  menu_lora_profile_acb },
  { MT_ADV_CALLBACK, 2, "869",  menu_lora_profile_acb },
  { MT_ADV_CALLBACK, 3, "915",  menu_lora_profile_acb },
  { MT_CALLBACK,     0, "QUICK CAL", menu_qcal_cb },
  { MT_NEXT, 0, NULL, menu_back } // next-> menu_back
};

//=====================================================================================================
// Shell command: profile [wide|433|869|915]
//=====================================================================================================
int shell_printf(const char *fmt, ...);
static void lora_profile_print(int i) {
  const lora_profile_t *p = &lora_profiles[i];
  shell_printf("%s %u %u %d %d %d\r\n", p->name, p->start, p->stop, p->slot,
               lora_profile_has_cal(p) ? 1 : 0, lora_profile_active(p) ? 1 : 0);
}

void cmd_profile(int argc, char *argv[]) {
  int i;
  if (argc == 0) {
    for (i = 0; i < (int)LORA_PROFILE_COUNT; i++)
      lora_profile_print(i);
    return;
  }
  if (argc == 1 && (i = get_str_index(argv[0], LORA_PROFILE_NAMES)) >= 0) {
    lora_profile_apply(i);
    lora_profile_print(i);
    return;
  }
  shell_printf("usage: profile [" LORA_PROFILE_NAMES "]\r\n");
}
#endif // __VNA_LORA_PROFILES__

/*
 * LoRa antenna profiles: plot side (included from plot.c after measure.c)
 *  - trace autoscale with hysteresis (SWR / LOGMAG / X), see lora_autoscale()
 *  - "LORA INFO" measure mode: resonance, SWR, RL, Z, SWR<=2 bandwidth, offset
 *
 * This is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3, or (at your option) any later version.
 */
#ifdef __VNA_LORA_PROFILES__

//=====================================================================================================
// Autoscale: bit t set = trace t is autoscaled. Cleared by set_trace_scale()/set_trace_refpos()
// (any manual scale/ref change), set by profile apply or the PROFILES->AUTOSCALE toggle.
//=====================================================================================================
uint8_t lora_autoscale_mask = 0;

// "Nice" scale steps (per grid division)
static const float as_swr[] = {0.125f, 0.25f, 0.5f, 1.0f, 2.0f};   // SWR 1..2, 3, 5, 9, 17
static const float as_db[]  = {1.0f, 2.0f, 3.0f, 5.0f, 10.0f};     // 0..-8 .. -80 dB
static const float as_ohm[] = {5.0f, 10.0f, 20.0f, 50.0f, 100.0f, 200.0f}; // +-20 .. +-800 Ohm

static int as_pick(const float *tbl, int n, float divs, float target) {
  int i;
  for (i = 0; i < n - 1; i++)
    if (tbl[i] * divs >= target) break;
  return i;
}

static void lora_autoscale(void) {
  if (lora_autoscale_mask == 0 || (props_mode & DOMAIN_MODE) == DOMAIN_TIME) return;
  for (int t = 0; t < TRACES_MAX; t++) {
    if (!(lora_autoscale_mask & (1<<t)) || !trace[t].enabled) continue;
    const float *tbl; int n; float divs, ref;
    switch (trace[t].type) {
      case TRC_SWR:    tbl = as_swr; n = ARRAY_COUNT(as_swr); divs = NGRIDY;   ref = 0;        break;
      case TRC_LOGMAG: tbl = as_db;  n = ARRAY_COUNT(as_db);  divs = NGRIDY;   ref = NGRIDY;   break;
      case TRC_X:      tbl = as_ohm; n = ARRAY_COUNT(as_ohm); divs = NGRIDY/2; ref = NGRIDY/2; break;
      default: continue;
    }
    get_value_cb_t c = trace_info_list[trace[t].type].get_value_cb;
    float (*array)[2] = measured[trace[t].channel];
    // lo/hi = min/max of the plotted quantity (SWR-1, -dB, |X|)
    float lo = 1e9f, hi = 0.0f;
    for (int i = 0; i < sweep_points; i++) {
      float v = c(i, array[i]);
      if (v != v) continue;                       // NaN
      if (trace[t].type == TRC_SWR)        v = v - 1.0f;          // INF -> big
      else if (trace[t].type == TRC_LOGMAG) v = -v;                // -INF (perfect match) -> big
      else                                  v = vna_fabsf(v);
      if (v > 1e6f) v = 1e6f;
      if (v < 0.0f) v = 0.0f;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    if (lo > hi) continue;                         // no valid data
    float target;
    if (trace[t].type == TRC_LOGMAG)               // fit the dip depth
      target = hi;
    else {                                         // fit the curve but cap far off-resonance values,
      float cap = (trace[t].type == TRC_SWR) ? 4.0f : 400.0f;  // keep the minimum visible if above cap
      target = hi < cap ? hi : cap;
      if (target < 2.0f * lo) target = 2.0f * lo;
    }
    // Hysteresis: grow at once, shrink only if target fits the smaller scale with 25% margin
    int cur = -1;
    for (int i = 0; i < n; i++) if (tbl[i] == trace[t].scale) cur = i;
    int up   = as_pick(tbl, n, divs, target);
    int down = as_pick(tbl, n, divs, target * 1.25f);
    int sel  = (cur < 0 || up > cur) ? up : (down < cur ? down : cur);
    if (trace[t].scale != tbl[sel] || trace[t].refpos != ref) {
      trace[t].scale  = tbl[sel];
      trace[t].refpos = ref;
      request_to_redraw(REDRAW_AREA);
    }
  }
}

//=====================================================================================================
// LORA INFO measure mode
//=====================================================================================================
typedef struct {
  float   f_res;      // resonance (SWR minimum, parabolic interpolated), 0 if no data
  float   swr, rl, r, x;
  float   f_lo, f_hi; // SWR<=2 edges
  uint8_t bw;         // bit0: SWR<=2 range found, bit1: open at left, bit2: open at right
} lora_measure_t;
static lora_measure_t *lora_meas = (lora_measure_t *)measure_memory;

static float lora_g2(uint16_t i) { return get_l(measured[0][i][0], measured[0][i][1]); }

// Frequency where |G|^2 crosses th between points a and b (linear)
static float lora_cross(int a, int b, float th) {
  float ya = lora_g2(a), yb = lora_g2(b);
  float k = (ya == yb) ? 0.0f : (th - ya) / (yb - ya);
  return (float)getFrequency(a) + k * ((float)getFrequency(b) - (float)getFrequency(a));
}

static void prepare_lora_info(uint8_t type, uint8_t update_mask) {
  (void)type;
  if (update_mask & MEASURE_UPD_SWEEP) {
    lora_measure_t *m = lora_meas;
    int n = sweep_points, i, x = 0;
    float y = lora_g2(0);
    for (i = 1; i < n; i++) { float v = lora_g2(i); if (v < y) {y = v; x = i;} }
    // Parabolic refine of the minimum
    float d = 0.0f;
    if (x > 0 && x < n - 1) {
      float y1 = lora_g2(x-1), y3 = lora_g2(x+1), den = y1 - 2.0f * y + y3;
      if (den > 0.0f) d = 0.5f * (y1 - y3) / den;
      if (d > 0.5f) d = 0.5f; else if (d < -0.5f) d = -0.5f;
    }
    m->f_res = (float)getFrequency(x) + d * (float)getFrequencyStep();
    m->swr = swr(x, measured[0][x]);
    m->rl  = -logmag(x, measured[0][x]);
    m->r   = resistance(x, measured[0][x]);
    m->x   = reactance(x, measured[0][x]);
    // SWR <= 2  <=>  |G| <= 1/3  <=>  |G|^2 <= 1/9
    const float th = 1.0f / 9.0f;
    m->bw = 0;
    if (y <= th) {
      m->bw = 1;
      for (i = x; i > 0 && lora_g2(i-1) <= th; i--);
      if (i == 0) {m->bw|= 2; m->f_lo = (float)getFrequency(0);} else m->f_lo = lora_cross(i-1, i, th);
      for (i = x; i < n-1 && lora_g2(i+1) <= th; i++);
      if (i == n-1) {m->bw|= 4; m->f_hi = (float)getFrequency(n-1);} else m->f_hi = lora_cross(i, i+1, th);
    }
  }
  invalidate_rect(STR_MEASURE_X                        , STR_MEASURE_Y,
                  STR_MEASURE_X + 5 * STR_MEASURE_WIDTH, STR_MEASURE_Y + 5 * STR_MEASURE_HEIGHT);
}

static void draw_lora_info(int xp, int yp) {
  const lora_measure_t *m = lora_meas;
  freq_t target = 0;
  bool cal_ok = true;
  const char *title = lora_profile_title(&target, &cal_ok);
  if (title) {
    if (!cal_ok) lcd_set_foreground(LCD_LOW_BAT_COLOR);
    cell_printf(xp, yp, cal_ok ? "%s" : "%s  NOT CALIBRATED - use QUICK CAL", title);
    lcd_set_foreground(LCD_MEASURE_COLOR);
    yp += STR_MEASURE_HEIGHT;
  }
  cell_printf(xp, yp, "Res %.6F" S_Hz "  SWR %.2f  RL %.1f" S_dB, m->f_res, m->swr, m->rl);
  yp += STR_MEASURE_HEIGHT;
  cell_printf(xp, yp, "Z %.1f%+j.1f" S_OHM, m->r, m->x);
  if (m->bw)
    cell_printf(xp + 2 * STR_MEASURE_WIDTH, yp, "BW(SWR<2) %s%.3F" S_Hz,
                (m->bw & 6) ? ">" : "", m->f_hi - m->f_lo);
  else
    cell_printf(xp + 2 * STR_MEASURE_WIDTH, yp, "BW(SWR<2) none");
  if (target) {
    yp += STR_MEASURE_HEIGHT;
    cell_printf(xp, yp, "Res-%.6F" S_Hz ": %+.3F" S_Hz, (float)target, m->f_res - (float)target);
  }
}
#endif // __VNA_LORA_PROFILES__

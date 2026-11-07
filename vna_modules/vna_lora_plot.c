/*
 * LoRa antenna profiles: plot side (included from plot.c after measure.c)
 *  - trace autoscale with hysteresis (SWR / LOGMAG / X), see lora_autoscale()
 *  - "LORA INFO" measure mode: resonance, SWR, RL, Z, SWR<=2 bandwidth, Q, offset
 *  - profile screen (profile range + LORA INFO): ISM band shading, SWR 1.5/2.0 guide
 *    lines, bold SWR trace, marker plates only on the SWR (and Smith) trace, M1 on the
 *    exact |S11| minimum, terse SWR labels for the band markers at the top of the plot,
 *    pass/fail + cal chips, and an info block placed in the emptiest plot corner.
 *    The stock multi-line marker readout at the top is not drawn on the profile screen.
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
// Profile screen state, updated before plot (plot_into_index) and before each draw (draw_all_cells)
//=====================================================================================================
#define LORA_SHADE_COLOR   RGB565(  0, 40, 72)   // ISM band background
#define LORA_GUIDE15_COLOR RGB565(  0,150,  0)   // SWR 1.5 guide
#define LORA_GUIDE20_COLOR RGB565(210,120,  0)   // SWR 2.0 guide
#define LORA_BOX_W         (29 * FONT_WIDTH)     // info block width
#define LORA_LINE_H        STR_MEASURE_HEIGHT
#define LORA_LBL_Y         2                     // band marker labels: 2 rows at the top
#define LORA_LBL_H         (2 * FONT_STR_HEIGHT + 2)
#define LORA_BAND_MAX      3

static const float lora_guide_swr[2] = {1.5f, 2.0f};

static struct {
  lora_screen_t s;
  int16_t band_x[LORA_BAND_MAX][2];   // shaded columns (area x, inclusive)
  int16_t guide_y[2];                 // SWR 1.5 / 2.0 line y, -1 = off screen
  int16_t box_x, box_y, box_h;        // info block
  uint8_t corner;                     // 0..3: TL TR BL BR
  uint8_t lines;                      // info block line count
  int16_t lbl_x[LORA_MK_MAX];         // band marker labels
  uint8_t lbl_row[LORA_MK_MAX];
  char    lbl[LORA_MK_MAX][12];
  float   lbl_swr[LORA_MK_MAX];       // < 0: label off
} lp;

static void lora_screen_update(void) {
  lora_screen_on = (props_mode & DOMAIN_MODE) == DOMAIN_FREQ && current_props._measure == MEASURE_LORA_INFO
                && lora_profile_screen(&lp.s);
  if (!lora_screen_on) return;
  // Band columns
  float f0 = (float)frequency0, k = (float)WIDTH / (float)(frequency1 - frequency0);
  for (int i = 0; i < lp.s.nband && i < LORA_BAND_MAX; i++) {
    int x0 = CELLOFFSETX + (int)(((float)lp.s.band[i][0] - f0) * k + 0.5f);
    int x1 = CELLOFFSETX + (int)(((float)lp.s.band[i][1] - f0) * k + 0.5f);
    if (x0 < CELLOFFSETX) x0 = CELLOFFSETX;
    if (x1 > CELLOFFSETX + WIDTH) x1 = CELLOFFSETX + WIDTH;
    lp.band_x[i][0] = x0; lp.band_x[i][1] = x1;   // x1 < x0: out of view
  }
  // SWR guide lines on the SWR trace scale (same math as trace_into_index)
  for (int g = 0; g < 2; g++) {
    lp.guide_y[g] = -1;
    if (!trace[0].enabled || trace[0].type != TRC_SWR) continue;
    float dscale = GRIDY / get_trace_scale(0);
    int y = HEIGHT - get_trace_refpos(0) * GRIDY + 0.5f - (lora_guide_swr[g] - 1.0f) * dscale;
    if (y > 0 && y < HEIGHT) lp.guide_y[g] = y;
  }
}

// Cell helpers
static void lora_cell_fill(int x, int y, int w, int h, pixel_t c) {
  if (x < 0) {w+= x; x = 0;}
  if (y < 0) {h+= y; y = 0;}
  if (x + w > CELLWIDTH)  w = CELLWIDTH  - x;
  if (y + h > CELLHEIGHT) h = CELLHEIGHT - y;
  for (; h > 0; h--, y++)
    for (int i = 0; i < w; i++) cell_buffer[y * CELLWIDTH + x + i] = c;
}

// Text width in pixels (same advance as put_normal), same on every cell
static int lora_str_width(const char *s) {
  int w = 0;
  for (; *s; s++) if ((uint8_t)*s >= FONT_START_CHAR) w+= FONT_GET_WIDTH((uint8_t)*s);
  return w;
}

// Print string, return its width
static int lora_print(int x, int y, const char *s) {
  cell_printf(x, y, "%s", s);
  return lora_str_width(s);
}

// SWR quality colour: green <= 1.5, yellow <= 2.0, red above
static pixel_t lora_swr_color(float v) {
  return GET_PALETTE_COLOR(v <= 1.5f ? LCD_NORMAL_BAT_COLOR : v <= 2.0f ? LCD_TRACE_1_COLOR : LCD_LOW_BAT_COLOR);
}

static void lora_fmt_swr(char *buf, int size, float v) {
  if (!(v < 99.5f)) plot_printf(buf, size, "99+");
  else              plot_printf(buf, size, v >= 9.995f ? "%.1f" : "%.2f", v);
}

// Called after the cell is cleared, before grid: ISM band shading
static void lora_cell_background(int x0, int w, int h) {
  if (!lora_screen_on) return;
  for (int i = 0; i < lp.s.nband && i < LORA_BAND_MAX; i++) {
    int xa = lp.band_x[i][0] - x0, xb = lp.band_x[i][1] - x0 + 1;
    if (xa < 0) xa = 0;
    if (xb > w) xb = w;
    if (xa < xb) lora_cell_fill(xa, 0, xb - xa, h, LORA_SHADE_COLOR);
  }
}

// Called after grid, before traces: dashed SWR 1.5 / 2.0 lines with labels at the left
static void lora_cell_guides(int x0, int y0, int w) {
  if (!lora_screen_on) return;
  for (int g = 0; g < 2; g++) {
    if (lp.guide_y[g] < 0) continue;
    pixel_t c = g ? LORA_GUIDE20_COLOR : LORA_GUIDE15_COLOR;
    int y = lp.guide_y[g] - y0;
    if ((uint32_t)y < CELLHEIGHT) {
      int xa = CELLOFFSETX - x0, xb = CELLOFFSETX + WIDTH - x0;
      if (xa < 0) xa = 0;
      if (xb > w) xb = w;
      for (int x = xa; x < xb; x++)
        if (((x + x0) & 7) < 4) cell_buffer[y * CELLWIDTH + x] = c;
    }
    // Label above the line, 1.5 label only if it does not collide with the 2 label
    if (g == 0 && lp.guide_y[1] >= 0 && lp.guide_y[0] - lp.guide_y[1] < FONT_STR_HEIGHT + 2) continue;
    foreground_color = c;
    cell_printf(CELLOFFSETX + 3 - x0, y - FONT_GET_HEIGHT - 1, g ? "2" : "1.5");
  }
}

// Profile screen marker glyphs (marker set 2, 11x14): R (resonance, M1 TRACK), L / H (band edges)
// Each: plate below the point (down) then plate above the point (up), drawn over the marker plate.
#if _USE_MARKER_SET_ == 2
#define LORA_GLYPH_R 0
#define LORA_GLYPH_L 1
#define LORA_GLYPH_H 2
static const uint8_t lora_marker_glyphs[] = {
  // R
  _BMP16(0b1111111111100000),
  _BMP16(0b1000000000100000),
  _BMP16(0b1001111100100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1001111100100000),
  _BMP16(0b1001111000100000),
  _BMP16(0b1001101100100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b0100000001000000),
  _BMP16(0b0010000010000000),
  _BMP16(0b0001000100000000),
  _BMP16(0b0000101000000000),

  _BMP16(0b0000101000000000),
  _BMP16(0b0001000100000000),
  _BMP16(0b0010000010000000),
  _BMP16(0b0100000001000000),
  _BMP16(0b1001111100100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1001111100100000),
  _BMP16(0b1001111000100000),
  _BMP16(0b1001101100100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1000000000100000),
  _BMP16(0b1111111111100000),
  // L
  _BMP16(0b1111111111100000),
  _BMP16(0b1000000000100000),
  _BMP16(0b1001100000100000),
  _BMP16(0b1001100000100000),
  _BMP16(0b1001100000100000),
  _BMP16(0b1001100000100000),
  _BMP16(0b1001100000100000),
  _BMP16(0b1001100000100000),
  _BMP16(0b1001100000100000),
  _BMP16(0b1001111110100000),
  _BMP16(0b0100000001000000),
  _BMP16(0b0010000010000000),
  _BMP16(0b0001000100000000),
  _BMP16(0b0000101000000000),

  _BMP16(0b0000101000000000),
  _BMP16(0b0001000100000000),
  _BMP16(0b0010000010000000),
  _BMP16(0b0100000001000000),
  _BMP16(0b1001100000100000),
  _BMP16(0b1001100000100000),
  _BMP16(0b1001100000100000),
  _BMP16(0b1001100000100000),
  _BMP16(0b1001100000100000),
  _BMP16(0b1001100000100000),
  _BMP16(0b1001100000100000),
  _BMP16(0b1001111110100000),
  _BMP16(0b1000000000100000),
  _BMP16(0b1111111111100000),
  // H
  _BMP16(0b1111111111100000),
  _BMP16(0b1000000000100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1001111110100000),
  _BMP16(0b1001111110100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b0100000001000000),
  _BMP16(0b0010000010000000),
  _BMP16(0b0001000100000000),
  _BMP16(0b0000101000000000),

  _BMP16(0b0000101000000000),
  _BMP16(0b0001000100000000),
  _BMP16(0b0010000010000000),
  _BMP16(0b0100000001000000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1001111110100000),
  _BMP16(0b1001111110100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1001100110100000),
  _BMP16(0b1000000000100000),
  _BMP16(0b1111111111100000),
};
#endif

// Marker glyph on the profile screen, NULL = stock number. M1: R while tracking the minimum,
// stock "1" when FREE; band profiles: M2 = L (low edge), M3 = H (high edge); WIDE: stock numbers.
static const uint8_t *lora_marker_glyph(int m, bool up) {
#if _USE_MARKER_SET_ == 2
  if (!lora_screen_on) return NULL;
  int g;
  if (m == 0) {
    if (!(props_mode & TD_MARKER_TRACK)) return NULL;
    g = LORA_GLYPH_R;
  } else if (lp.s.mk == NULL && m <= 2)
    g = m == 1 ? LORA_GLYPH_L : LORA_GLYPH_H;
  else
    return NULL;
  return &lora_marker_glyphs[(g * 4 + (up ? 2 : 0)) * MARKER_HEIGHT];
#else
  (void)m; (void)up;
  return NULL;
#endif
}

// Draw marker plate of trace t on the profile screen? (only SWR, current and Smith)
static inline bool lora_marker_on_trace(int t) {
  return !lora_screen_on || t == 0 || t == current_trace || trace[t].type == TRC_SMITH;
}

// ui.c side
bool lora_screen_active(void) {return lora_screen_on;}
bool lora_marker_shown(int t) {return lora_marker_on_trace(t);}

//=====================================================================================================
// LORA INFO measure mode
//=====================================================================================================
typedef struct {
  float   f_res;      // resonance (SWR minimum, parabolic interpolated), 0 if no data
  float   swr, rl, r, x;
  float   f_lo, f_hi; // SWR<=2 edges
  float   worst;      // worst SWR inside the band (band profile), < 0 = none
  float   ml;         // mismatch loss at the minimum (dB)
  float   dip_f[3], dip_swr[3]; // WIDE/SURVEY: best dips
  uint16_t res_idx;   // point of the minimum (M1 tracking)
  uint8_t bw;         // bit0: SWR<=2 range found, bit1: open at left, bit2: open at right
  uint8_t flag;       // LORA_FLAG_xx antenna sanity
  uint8_t ndip;
} lora_measure_t;
enum {LORA_FLAG_OK = 0, LORA_FLAG_OPEN, LORA_FLAG_SHORT, LORA_FLAG_LOSSY};
static const char *lora_flag_names[] = {"ok", "open", "short", "lossy"};

// Mismatch loss (dB) from |G|^2
static float lora_ml(float g2) {return g2 < 1.0f ? -vna_log10f_x_10(1.0f - g2) : 99.0f;}
static lora_measure_t *lora_meas = (lora_measure_t *)measure_memory;

static float lora_g2(uint16_t i) { return get_l(measured[0][i][0], measured[0][i][1]); }

// Frequency where |G|^2 crosses th between points a and b (linear)
static float lora_cross(int a, int b, float th) {
  float ya = lora_g2(a), yb = lora_g2(b);
  float k = (ya == yb) ? 0.0f : (th - ya) / (yb - ya);
  return (float)getFrequency(a) + k * ((float)getFrequency(b) - (float)getFrequency(a));
}

static uint16_t lora_min_index(void) {
  uint16_t x = 0;
  float y = lora_g2(0);
  for (int i = 1; i < sweep_points; i++) { float v = lora_g2(i); if (v < y) {y = v; x = i;} }
  return x;
}

// Marker tracking on the profile screen: M1 on the exact |S11| minimum (stock search uses the
// pixel y of the current trace, so it stops on the first point of a flat clipped bottom), band
// markers stay where the profile put them. Return false if not on the profile screen.
static bool lora_track(void) {
  if (!lora_screen_on) return false;
  if (markers[0].enabled)
    set_marker_index(0, lora_min_index());
  return true;
}

// Antenna sanity: |G| > 0.95 on every point = nothing connected (open) or a short; on a wide
// sweep, SWR < 2 on most points with little reactance = resistive load / lossy fake antenna
static void lora_sanity(lora_measure_t *m) {
  int n = sweep_points, high = 0, low = 0;
  float re = 0.0f, xmax = 0.0f;
  for (int i = 0; i < n; i++) {
    float g2 = lora_g2(i);
    if (g2 > 0.9025f) high++;
    if (g2 < 1.0f / 9.0f) {
      low++;
      float x = vna_fabsf(reactance(i, measured[0][i]));
      if (x > xmax) xmax = x;
    }
    re+= measured[0][i][0];
  }
  m->flag = LORA_FLAG_OK;
  if (high == n)
    m->flag = re > 0.0f ? LORA_FLAG_OPEN : LORA_FLAG_SHORT;
  else if (lp.s.nband > 1 && low * 10 >= n * 6 && xmax < 30.0f)
    m->flag = LORA_FLAG_LOSSY;
}

// Prominence walk from local minimum i in direction dir: true if |G|^2 rises by LORA_DIP_RISE before
// a deeper point is met (sweep edge counts as risen, a dip at the edge of the range is still a dip)
#define LORA_DIP_RISE 0.05f
static bool lora_dip_side(int i, int dir) {
  float g = lora_g2(i), top = g;
  for (int j = i + dir; j >= 0 && j < sweep_points; j+= dir) {
    float v = lora_g2(j);
    if (v < g) return false;
    if (v >= g + LORA_DIP_RISE) return true;
    if (v > top) top = v;
  }
  return top >= g + 0.5f * LORA_DIP_RISE;         // at the sweep edge: half the rise is enough
}

// WIDE/SURVEY: up to 3 dips (local |G| minimum below SWR 3 with a prominence of 0.05 |G|^2 on both
// sides), best first
static void lora_dips(lora_measure_t *m) {
  m->ndip = 0;
  if (!lora_screen_on || lp.s.nband < 2) return;
  int n = sweep_points, idx[3];
  for (int i = 0; i < n; i++) {
    float g = lora_g2(i);
    if (g >= 0.25f || (i > 0 && g >= lora_g2(i-1)) || (i < n - 1 && g > lora_g2(i+1))) continue;
    if (!lora_dip_side(i, -1) || !lora_dip_side(i, 1)) continue;
    // insert sorted by depth
    int k = m->ndip < 3 ? m->ndip++ : 3;
    while (k > 0 && lora_g2(idx[k-1]) > g) {if (k < 3) idx[k] = idx[k-1]; k--;}
    if (k < 3) idx[k] = i;
  }
  for (int k = 0; k < m->ndip; k++) {
    m->dip_f[k]   = (float)getFrequency(idx[k]);
    m->dip_swr[k] = swr(idx[k], measured[0][idx[k]]);
  }
}

// Nearest LoRa band centre (MHz) to f
static uint32_t lora_near_band(float f) {
  static const uint16_t c[] = {433, 868, 915};
  int b = 0;
  for (int k = 1; k < 3; k++) if (vna_fabsf(f - c[k] * 1e6f) < vna_fabsf(f - c[b] * 1e6f)) b = k;
  return c[b];
}

//=====================================================================================================
// REF: S11 reference (|G|^2 per point + results), RAM only (CCM), drawn dim grey in the stored trace slot
//=====================================================================================================
#define LORA_REF_COLOR RGB565(120,120,120)
typedef struct {
  freq_t   f0, f1;
  uint16_t points;
  bool     valid, shown;
  float    f_res, swr, bw;
  float    g2[SWEEP_POINTS_MAX];
} lora_ref_t;
static lora_ref_t lora_ref LORA_CCM;

static bool lora_ref_here(void) {
  return lora_ref.valid && lora_ref.f0 == frequency0 && lora_ref.f1 == frequency1 && lora_ref.points == sweep_points;
}
bool lora_ref_valid(void) {return lora_ref_here();}   // stored for the current range
float lora_res_freq(void) {return lora_meas->f_res;}

bool lora_ref_store(void) {
  if (!lora_screen_on) return false;
  const lora_measure_t *m = lora_meas;
  for (int i = 0; i < sweep_points; i++) lora_ref.g2[i] = lora_g2(i);
  lora_ref.f0 = frequency0; lora_ref.f1 = frequency1; lora_ref.points = sweep_points;
  lora_ref.f_res = m->f_res; lora_ref.swr = m->swr;
  lora_ref.bw = (m->bw && !(m->bw & 6)) ? m->f_hi - m->f_lo : 0.0f;
  lora_ref.valid = true;
  request_to_redraw(REDRAW_PLOT | REDRAW_FREQUENCY);
  return true;
}

void lora_ref_clear(void) {
  lora_ref.valid = false;
  request_to_redraw(REDRAW_PLOT | REDRAW_AREA | REDRAW_FREQUENCY);
}

// plot_into_index(): REF curve on the SWR trace scale in the stored trace slot
static void lora_ref_into_index(void) {
#if STORED_TRACES > 0
  bool show = lora_screen_on && lora_ref_here() && trace[0].enabled && trace[0].type == TRC_SWR;
  if (!show) {
    if (lora_ref.shown) {enabled_store_trace&= ~1; lora_ref.shown = false; request_to_redraw(REDRAW_AREA);}
    return;
  }
  index_t *index = trace_index[TRACES_MAX];
  float dscale = GRIDY / get_trace_scale(0);
  float refpos = HEIGHT - get_trace_refpos(0) * GRIDY + 0.5f + dscale;
  uint32_t dx = ((WIDTH)<<16) / (sweep_points-1), x = (CELLOFFSETX<<16) + 0x8000;
  for (int i = 0; i < sweep_points; i++, x+= dx) {
    float g = vna_sqrtf(lora_ref.g2[i]);
    int y = 0;
    if (g < 0.999f) {
      y = refpos - (1.0f + g) / (1.0f - g) * dscale;
      if (y < 0) y = 0; else if (y > HEIGHT) y = HEIGHT;
    }
    mark_set_index(index, i, (uint16_t)(x>>16), y);
  }
  stored_traces_points[0] = sweep_points;
  enabled_store_trace|= 1;
  lora_ref.shown = true;
#endif
}

//=====================================================================================================
// Toast: 1 s message over the plot (profile screen)
//=====================================================================================================
#define LORA_TOAST_W 260
#define LORA_TOAST_H (FONT_STR_HEIGHT + 10)
#define LORA_TOAST_X (CELLOFFSETX + (WIDTH - LORA_TOAST_W) / 2)
#define LORA_TOAST_Y 44
static struct {char text[32]; systime_t start; bool on;} lora_toast_s LORA_CCM;

static void lora_toast_invalidate(void) {
  invalidate_rect(LORA_TOAST_X, LORA_TOAST_Y, LORA_TOAST_X + LORA_TOAST_W, LORA_TOAST_Y + LORA_TOAST_H);
}

void lora_toast(const char *text) {
  strncpy(lora_toast_s.text, text, sizeof(lora_toast_s.text) - 1);
  lora_toast_s.start = chVTGetSystemTimeX();
  lora_toast_s.on = true;
  lora_toast_invalidate();
  request_to_redraw(REDRAW_CELLS);
}

static void lora_cell_toast(int x0, int y0) {
  if (!lora_toast_s.on) return;
  int x = LORA_TOAST_X - x0, y = LORA_TOAST_Y - y0;
  if (x >= CELLWIDTH || x + LORA_TOAST_W <= 0 || y >= CELLHEIGHT || y + LORA_TOAST_H <= 0) return;
  lora_cell_fill(x, y, LORA_TOAST_W, LORA_TOAST_H, GET_PALETTE_COLOR(LCD_FG_COLOR));
  lora_cell_fill(x + 2, y + 2, LORA_TOAST_W - 4, LORA_TOAST_H - 4, GET_PALETTE_COLOR(LCD_MENU_COLOR));
  lcd_set_foreground(LCD_MENU_TEXT_COLOR);
  cell_text_shadow = false;
  cell_printf(x + (LORA_TOAST_W - lora_str_width(lora_toast_s.text)) / 2, y + 5, "%s", lora_toast_s.text);
  cell_text_shadow = true;
}

//=====================================================================================================
// TUNE: full screen big digits (narrow 101 point sweep set by the caller)
//=====================================================================================================
static struct {lora_tune_t p; bool on; uint8_t skip;} lora_tune_s LORA_CCM;

void lora_tune_start(const lora_tune_t *t) {
  lora_tune_s.p = *t;
  lora_tune_s.on = true;
  lora_tune_s.skip = 1;                  // first completed sweep may be the old range
  request_to_redraw(REDRAW_CLRSCR);
}

void lora_tune_stop(void) {
  lora_tune_s.on = false;
  request_to_redraw(REDRAW_CLRSCR | REDRAW_AREA | REDRAW_FREQUENCY | REDRAW_CAL_STATUS | REDRAW_BATTERY);
}

bool lora_tune_active(void) {return lora_tune_s.on;}

// Scaled text width; '.' is drawn as a square (the bold font dot is a small cross, a '+' when scaled)
static int lora_text_w(const char *s, int size) {
  int w = 0;
  for (; *s; s++) w+= (*s == '.' && size > 1) ? 3 * size : FONT_GET_WIDTH((uint8_t)*s) * size;
  return w;
}

// Text with scaled font, x < 0: centred
static int lora_big(int x, int y, const char *s, int size, uint16_t fg_idx) {
  if (x < 0) x = (LCD_WIDTH - lora_text_w(s, size)) / 2;
  lcd_set_colors(fg_idx, LCD_BG_COLOR);
  for (; *s; s++) {
    if (*s == '.' && size > 1) {
      pixel_t bg = background_color;
      background_color = foreground_color;
      lcd_fill(x + size / 2, y + 7 * size + size / 2, 2 * size, 2 * size);
      background_color = bg;
      x+= 3 * size;
    } else
      x+= lcd_drawchar_size(*s, x, y, size);
  }
  return x;
}

// Tune results from the current sweep
typedef struct {float f_res, swr_res, g2_t, swr_t; bool ok;} lora_tune_res_t;   // ok: dip inside the range
static lora_tune_res_t lora_tune_calc(void) {
  lora_tune_res_t r;
  int n = sweep_points, x = lora_min_index();
  float y = lora_g2(x), d = 0.0f;
  if (x > 0 && x < n - 1) {
    float y1 = lora_g2(x-1), y3 = lora_g2(x+1), den = y1 - 2.0f * y + y3;
    if (den > 0.0f) d = 0.5f * (y1 - y3) / den;
    if (d > 0.5f) d = 0.5f; else if (d < -0.5f) d = -0.5f;
  }
  r.f_res   = (float)getFrequency(x) + d * (float)getFrequencyStep();
  r.swr_res = swr(x, measured[0][x]);
  r.ok = x > 0 && x < n - 1 && lora_dip_side(x, -1) && lora_dip_side(x, 1);
  float fi = ((float)lora_tune_s.p.target - (float)frequency0) / (float)getFrequencyStep();
  int i0 = (int)fi;
  if (i0 < 0) i0 = 0; else if (i0 > n - 2) i0 = n - 2;
  float k = fi - i0;
  r.g2_t  = lora_g2(i0) + k * (lora_g2(i0 + 1) - lora_g2(i0));
  float g = vna_sqrtf(r.g2_t);
  r.swr_t = g < 0.999f ? (1.0f + g) / (1.0f - g) : 999.0f;
  if (r.g2_t > 0.999f) r.g2_t = 0.999f;           // uncalibrated / no antenna: keep RL/ML sane
  return r;
}

static void lora_tune_static(void) {
  lcd_set_colors(LCD_FG_COLOR, LCD_BG_COLOR);
  lcd_clear_screen();
  char buf[48];
  plot_printf(buf, sizeof(buf), "TUNE  %s  target %.3f MHz", lora_tune_s.p.title, (float)lora_tune_s.p.target * 1e-6f);
  lora_big(8, 6, buf, 1, LCD_FG_COLOR);
  lora_big(LCD_WIDTH - 8 - lora_text_w("tap or push to exit", 1), 6, "tap or push to exit", 1, LCD_TRACE_1_COLOR);
  if (lora_tune_s.p.cal == LORA_CAL_NONE) lora_big(-1, 24, "NOT CALIBRATED", 1, LCD_LOW_BAT_COLOR);
  lora_big(8, 26, "RESONANCE", 1, LCD_GRID_COLOR);
  lora_big(-1, 110, "measuring...", 2, LCD_GRID_COLOR);
}

static void lora_tune_draw(void) {
  char buf[48];
  lora_tune_res_t r = lora_tune_calc();
  float ft = (float)lora_tune_s.p.target;
  float pct = (r.f_res - ft) / ft * 100.0f;
  // Big resonance frequency
  lcd_set_colors(LCD_FG_COLOR, LCD_BG_COLOR);
  lcd_fill(0, 40, LCD_WIDTH, 58);
  if (r.ok) plot_printf(buf, sizeof(buf), "%.2f", r.f_res * 1e-6f);
  else      plot_printf(buf, sizeof(buf), "---.--");
  int w = lora_text_w(buf, 5) + 10 + lora_text_w("MHz", 2);
  int x = lora_big((LCD_WIDTH - w) / 2, 40, buf, 5, LCD_FG_COLOR);
  lora_big(x + 10, 40 + 5 * FONT_GET_HEIGHT - 2 * FONT_GET_HEIGHT, "MHz", 2, LCD_FG_COLOR);
  // Direction: resonance below target = antenna too long = shorten
  lcd_fill(0, 100, LCD_WIDTH, 3 * FONT_GET_HEIGHT + FONT_STR_HEIGHT + 6);
  float apct = vna_fabsf(pct);
  uint16_t c = apct < 0.3f ? LCD_NORMAL_BAT_COLOR : apct < 2.0f ? LCD_TRACE_1_COLOR : LCD_LOW_BAT_COLOR;
  if (!r.ok) {c = LCD_LOW_BAT_COLOR; plot_printf(buf, sizeof(buf), "NO DIP IN RANGE");}
  else if (apct < 0.3f) plot_printf(buf, sizeof(buf), "ON TARGET");
  else plot_printf(buf, sizeof(buf), pct < 0.0f ? "<< SHORTEN %.1f %%" : "LENGTHEN %.1f %% >>", apct);
  lora_big(-1, 102, buf, 3, c);
  if (r.ok)
    plot_printf(buf, sizeof(buf), "resonance %.2f MHz %s target (%+.2f %%)", vna_fabsf(r.f_res - ft) * 1e-6f,
                pct < 0.0f ? "below" : "above", pct);
  else
    plot_printf(buf, sizeof(buf), "antenna far off tune, missing or not calibrated");
  lora_big(-1, 102 + 3 * FONT_GET_HEIGHT + 4, buf, 1, LCD_GRID_COLOR);
  // SWR / RL / ML at the target frequency
  int y = 158;
  lcd_set_colors(LCD_FG_COLOR, LCD_BG_COLOR);
  lcd_fill(0, y, LCD_WIDTH, 4 * FONT_GET_HEIGHT + FONT_STR_HEIGHT + 4);
  plot_printf(buf, sizeof(buf), "SWR at %.3f MHz", ft * 1e-6f);
  lora_big(8, y, buf, 1, LCD_GRID_COLOR);
  if (r.swr_t < 99.5f) plot_printf(buf, sizeof(buf), "%.2f", r.swr_t); else plot_printf(buf, sizeof(buf), "99+");
  c = r.swr_t <= 1.5f ? LCD_NORMAL_BAT_COLOR : r.swr_t <= 2.0f ? LCD_TRACE_1_COLOR : LCD_LOW_BAT_COLOR;
  lora_big(8, y + FONT_STR_HEIGHT + 2, buf, 4, c);
  plot_printf(buf, sizeof(buf), "RL %.1f dB", -vna_log10f_x_10(r.g2_t));
  lora_big(240, y + FONT_STR_HEIGHT + 2, buf, 2, LCD_FG_COLOR);
  plot_printf(buf, sizeof(buf), "ML %.2f dB", lora_ml(r.g2_t));
  lora_big(240, y + FONT_STR_HEIGHT + 4 + 2 * FONT_GET_HEIGHT, buf, 2, LCD_FG_COLOR);
  // REF: trim / detune feedback
  y = 222;
  lcd_set_colors(LCD_FG_COLOR, LCD_BG_COLOR);
  lcd_fill(0, y, LCD_WIDTH, 2 * FONT_GET_HEIGHT);
  if (lora_ref.valid && lora_ref.f0 == frequency0 && lora_ref.f1 == frequency1) {   // same band range
    plot_printf(buf, sizeof(buf), "REF %.2f > %.2f (%+.2f MHz)", lora_ref.f_res * 1e-6f, r.f_res * 1e-6f,
                (r.f_res - lora_ref.f_res) * 1e-6f);
    lora_big(-1, y, buf, 2, LCD_TRACE_2_COLOR);
  }
  // SWR strip: SWR 1 (bottom) .. 3 (top), band shaded, target line, SWR 2 dashed
  const int sx = 10, sw = LCD_WIDTH - 20, sy = 250, sh = LCD_HEIGHT - 4 - sy;
  lcd_set_background(LCD_BG_COLOR);
  lcd_fill(sx, sy, sw, sh);
  float k = (float)sw / (float)(frequency1 - frequency0);
  int bx0 = sx + ((float)lora_tune_s.p.band_lo - (float)frequency0) * k;
  int bx1 = sx + ((float)lora_tune_s.p.band_hi - (float)frequency0) * k;
  if (bx1 > bx0) {background_color = LORA_SHADE_COLOR; lcd_fill(bx0, sy, bx1 - bx0 + 1, sh);}
  lcd_set_foreground(LCD_GRID_COLOR);
  lcd_line(sx, sy, sx + sw, sy); lcd_line(sx, sy + sh, sx + sw, sy + sh);
  foreground_color = LORA_GUIDE20_COLOR;
  for (int xx = sx; xx < sx + sw; xx+= 8) lcd_line(xx, sy + sh / 2, xx + 3, sy + sh / 2);
  int tx = sx + (ft - (float)frequency0) * k;
  lcd_set_foreground(LCD_FG_COLOR);
  for (int yy = sy; yy < sy + sh; yy+= 6) lcd_line(tx, yy, tx, yy + 2);
  lcd_set_foreground(LCD_TRACE_1_COLOR);
  int px = 0, py = 0;
  for (int i = 0; i < sweep_points; i++) {
    float v = swr(i, measured[0][i]);
    int yy = sy + sh - (int)((v - 1.0f) * 0.5f * sh);
    if (!(v < 3.0f)) yy = sy;
    int xx = sx + i * sw / (sweep_points - 1);
    if (i) {lcd_line(px, py, xx, yy); lcd_line(px, py - 1, xx, yy - 1);}
    px = xx; py = yy;
  }
  lcd_set_colors(LCD_GRID_COLOR, LCD_BG_COLOR);
  lcd_drawstring(sx + 2, sy + 2, "SWR 3");
  lcd_drawstring(sx + 2, sy + sh - FONT_STR_HEIGHT, "1");
}

//=====================================================================================================
// HOLD: min/max envelope of |G| (= SWR and |S11|) per point since HOLD was switched on, drawn as a
// shaded band on the SWR scale, best SWR so far in the info block (CCM, RAM only)
//=====================================================================================================
#define LORA_HOLD_COLOR RGB565(96, 88, 0)
typedef struct {
  bool     on;
  freq_t   f0, f1;
  uint16_t points, sweeps;
  float    best_swr, best_f;
  uint16_t qmin[SWEEP_POINTS_MAX], qmax[SWEEP_POINTS_MAX];   // |G|^2 * 65535
  uint16_t ytop[SWEEP_POINTS_MAX], ybot[SWEEP_POINTS_MAX];   // pixels on the SWR trace scale
} lora_hold_t;
static lora_hold_t lora_hold LORA_CCM;

static void lora_hold_reset(void) {
  lora_hold.f0 = frequency0; lora_hold.f1 = frequency1; lora_hold.points = sweep_points;
  lora_hold.sweeps = 0; lora_hold.best_swr = 1e9f; lora_hold.best_f = 0.0f;
  for (int i = 0; i < SWEEP_POINTS_MAX; i++) {
    lora_hold.qmin[i] = 0xFFFF; lora_hold.qmax[i] = 0;
    lora_hold.ytop[i] = lora_hold.ybot[i] = 0;
  }
}

void lora_hold_set(bool on) {
  if (on) lora_hold_reset();                       // re-enable clears the envelope
  lora_hold.on = on;
  request_to_redraw(REDRAW_AREA | REDRAW_FREQUENCY);
}
bool lora_hold_active(void) {return lora_hold.on;}

// |G|^2 -> y on the SWR trace scale (0 = top)
static int lora_swr_y(float g2) {
  float g = vna_sqrtf(g2);
  if (g >= 0.999f) return 0;
  float dscale = GRIDY / get_trace_scale(0);
  int y = HEIGHT - get_trace_refpos(0) * GRIDY + 0.5f + dscale - (1.0f + g) / (1.0f - g) * dscale;
  return y < 0 ? 0 : y > HEIGHT ? HEIGHT : y;
}

// plot_into_index(): add the sweep to the envelope, mark changed columns for redraw
static void lora_hold_update(void) {
  if (!lora_hold.on || !lora_screen_on) return;
  if (lora_hold.f0 != frequency0 || lora_hold.f1 != frequency1 || lora_hold.points != sweep_points) {
    lora_hold_reset();
    request_to_redraw(REDRAW_AREA);
  }
  bool draw = trace[0].enabled && trace[0].type == TRC_SWR;
  for (int i = 0; i < sweep_points; i++) {
    float g2 = lora_g2(i);
    uint16_t q = g2 >= 1.0f ? 0xFFFF : (uint16_t)(g2 * 65535.0f);
    if (q < lora_hold.qmin[i]) {
      lora_hold.qmin[i] = q;
      float v = swr(i, measured[0][i]);
      if (v < lora_hold.best_swr) {lora_hold.best_swr = v; lora_hold.best_f = (float)getFrequency(i);}
    }
    if (q > lora_hold.qmax[i]) lora_hold.qmax[i] = q;
    if (!draw) continue;
    uint16_t yt = lora_swr_y(lora_hold.qmax[i] / 65535.0f), yb = lora_swr_y(lora_hold.qmin[i] / 65535.0f);
    if (yt != lora_hold.ytop[i] || yb != lora_hold.ybot[i]) {
      int x = CELLOFFSETX + (i * WIDTH + (sweep_points - 1) / 2) / (sweep_points - 1);
      int a = yt < lora_hold.ytop[i] ? yt : lora_hold.ytop[i];
      int b = yb > lora_hold.ybot[i] ? yb : lora_hold.ybot[i];
      mark_line(x, a, x + 1, b);
      lora_hold.ytop[i] = yt; lora_hold.ybot[i] = yb;
    }
  }
  lora_hold.sweeps++;
}

// draw_cell(): shaded envelope (after band shading, before grid and traces)
static void lora_cell_hold(int x0, int y0, int w, int h) {
  if (!lora_hold.on || !lora_screen_on || lora_hold.sweeps == 0 || lora_hold.points != sweep_points
      || !trace[0].enabled || trace[0].type != TRC_SWR) return;
  int n = sweep_points;
  int i0 = (x0 - CELLOFFSETX - 2) * (n - 1) / WIDTH, i1 = (x0 + w - CELLOFFSETX + 1) * (n - 1) / WIDTH + 1;
  if (i0 < 0) i0 = 0;
  if (i1 > n - 1) i1 = n - 1;
  for (int i = i0; i <= i1; i++) {
    int x = CELLOFFSETX + (i * WIDTH + (n - 1) / 2) / (n - 1) - x0;
    int ya = lora_hold.ytop[i] - y0, yb = lora_hold.ybot[i] - y0 + 1;
    if (ya < 0) ya = 0;
    if (yb > h) yb = h;
    for (int xx = x; xx <= x + 1; xx++) {
      if ((uint32_t)xx >= (uint32_t)w) continue;
      for (int y = ya; y < yb; y++) cell_buffer[y * CELLWIDTH + xx] = LORA_HOLD_COLOR;
    }
  }
}

//=====================================================================================================
// S21 COMPARE: full screen, port 1 = fixed REF antenna, port 2 = antenna under test.
// Store antenna A (average of 3 sweeps), then the live in-band average of antenna B minus A.
// Relative only: no THRU calibration needed, keep cables and positions fixed.
//=====================================================================================================
#define LORA_CMP_POINTS  101
#define LORA_CMP_STORE_N 3
enum {LORA_CMP_OFF = 0, LORA_CMP_ASK_A, LORA_CMP_STORING, LORA_CMP_LIVE};
// Touch areas
#define LORA_CMP_BTN_W   150
#define LORA_CMP_BTN_H   44
#define LORA_CMP_BTN_Y   (LCD_HEIGHT - LORA_CMP_BTN_H - 6)
#define LORA_CMP_EXIT_X  (LCD_WIDTH - LORA_CMP_BTN_W - 8)
#define LORA_CMP_NEWA_X  8
typedef struct {
  lora_tune_t p;                     // title, band centre, band edges
  uint8_t state, count, skip;
  bool    a_valid, b_init;
  freq_t  a_f0, a_f1;
  float   a[LORA_CMP_POINTS], b[LORA_CMP_POINTS];   // |S21| dB
} lora_cmp_t;
static lora_cmp_t lora_cmp LORA_CCM;

static float lora_s21_db(int i) {
  float p = get_l(measured[1][i][0], measured[1][i][1]);
  return p > 1e-12f ? vna_log10f_x_10(p) : -120.0f;
}

void lora_cmp_start(const lora_tune_t *t) {
  lora_cmp.p = *t;
  lora_cmp.skip = 1;
  lora_cmp.b_init = false;
  // A stored before over the same range: straight to the live comparison
  lora_cmp.state = (lora_cmp.a_valid && lora_cmp.a_f0 == frequency0 && lora_cmp.a_f1 == frequency1) ? LORA_CMP_LIVE : LORA_CMP_ASK_A;
  request_to_redraw(REDRAW_CLRSCR);
}
void lora_cmp_stop(void) {
  lora_cmp.state = LORA_CMP_OFF;
  request_to_redraw(REDRAW_CLRSCR | REDRAW_AREA | REDRAW_FREQUENCY | REDRAW_CAL_STATUS | REDRAW_BATTERY);
}
bool lora_cmp_active(void) {return lora_cmp.state != LORA_CMP_OFF;}

void lora_cmp_store_a(void) {
  if (!lora_cmp_active()) return;
  for (int i = 0; i < LORA_CMP_POINTS; i++) lora_cmp.a[i] = 0.0f;
  lora_cmp.count = 0;
  lora_cmp.skip = 1;                                 // next complete sweep
  lora_cmp.state = LORA_CMP_STORING;
  request_to_redraw(REDRAW_CLRSCR);
}
void lora_cmp_new_a(void) {
  lora_cmp.a_valid = false;
  lora_cmp.state = LORA_CMP_ASK_A;
  request_to_redraw(REDRAW_CLRSCR);
}

// Touch action: 0 = exit, 1 = store A, 2 = new A, -1 = nothing
int lora_cmp_touch(int x, int y) {
  bool btn = y >= LORA_CMP_BTN_Y - 4;
  switch (lora_cmp.state) {
    case LORA_CMP_ASK_A:   return (btn && x >= LORA_CMP_EXIT_X) ? 0 : 1;
    case LORA_CMP_STORING: return -1;
    case LORA_CMP_LIVE:    return (btn && x < LORA_CMP_NEWA_X + LORA_CMP_BTN_W) ? 2 : 0;
  }
  return 0;
}

// In-band (band edges) power average of dB values in v (A or B), points of the current sweep
static float lora_cmp_band_avg(const float *v) {
  float s = 0.0f; int n = 0;
  for (int i = 0; i < LORA_CMP_POINTS && i < sweep_points; i++) {
    freq_t f = getFrequency(i);
    if (f < lora_cmp.p.band_lo || f > lora_cmp.p.band_hi) continue;
    s+= vna_expf(v[i] * 0.23025851f); n++;        // 10^(dB/10)
  }
  return n ? vna_log10f_x_10(s / n) : -120.0f;
}

typedef struct {float a, b, d, worst, best; bool ok;} lora_cmp_res_t;
static lora_cmp_res_t lora_cmp_calc(void) {
  lora_cmp_res_t r = {0};
  if (lora_cmp.state != LORA_CMP_LIVE || !lora_cmp.b_init) return r;
  r.a = lora_cmp_band_avg(lora_cmp.a);
  r.b = lora_cmp_band_avg(lora_cmp.b);
  r.d = r.b - r.a;
  r.worst = 1e9f; r.best = -1e9f;
  for (int i = 0; i < LORA_CMP_POINTS && i < sweep_points; i++) {
    freq_t f = getFrequency(i);
    if (f < lora_cmp.p.band_lo || f > lora_cmp.p.band_hi) continue;
    float d = lora_cmp.b[i] - lora_cmp.a[i];
    if (d < r.worst) r.worst = d;
    if (d > r.best)  r.best  = d;
  }
  r.ok = true;
  return r;
}

static void lora_cmp_button(int x, const char *text, uint16_t bg) {
  lcd_set_background(LCD_FG_COLOR);
  lcd_fill(x, LORA_CMP_BTN_Y, LORA_CMP_BTN_W, LORA_CMP_BTN_H);
  lcd_set_background(bg);
  lcd_fill(x + 2, LORA_CMP_BTN_Y + 2, LORA_CMP_BTN_W - 4, LORA_CMP_BTN_H - 4);
  lcd_set_foreground(LCD_FG_COLOR);
  lcd_drawstring_size(text, x + (LORA_CMP_BTN_W - lora_text_w(text, 2)) / 2, LORA_CMP_BTN_Y + (LORA_CMP_BTN_H - 2 * FONT_GET_HEIGHT) / 2, 2);
}

static void lora_cmp_static(void) {
  char buf[64];
  lcd_set_colors(LCD_FG_COLOR, LCD_BG_COLOR);
  lcd_clear_screen();
  plot_printf(buf, sizeof(buf), "S21 COMPARE  %s  band %.2f-%.2f MHz", lora_cmp.p.title,
              lora_cmp.p.band_lo * 1e-6f, lora_cmp.p.band_hi * 1e-6f);
  lora_big(8, 6, buf, 1, LCD_FG_COLOR);
  lora_big(LCD_WIDTH - 8 - lora_text_w("push to exit", 1), 6, "push to exit", 1, LCD_TRACE_1_COLOR);
  lora_big(-1, 22, "relative only: no THRU cal needed, keep cables and positions fixed", 1, LCD_GRID_COLOR);
  if (lora_cmp.state == LORA_CMP_LIVE) {
    lora_big(8, 40, "Antenna B on port 2 (swap freely)", 1, LCD_FG_COLOR);
    lora_cmp_button(LORA_CMP_NEWA_X, "NEW A", LCD_GRID_COLOR);
    lora_big(LORA_CMP_NEWA_X + LORA_CMP_BTN_W + 12, LORA_CMP_BTN_Y + 16, "tap elsewhere to exit", 1, LCD_GRID_COLOR);
    return;
  }
  lora_big(-1, 50, "Port 1: fixed REF antenna (tripod)", 2, LCD_FG_COLOR);
  lora_big(-1, 80, "Port 2: antenna to test", 2, LCD_FG_COLOR);
  if (lora_cmp.state == LORA_CMP_ASK_A) {
    lora_big(-1, 120, "Put antenna A on port 2", 2, LCD_TRACE_1_COLOR);
    lora_big(-1, 150, "TAP TO STORE A", 4, LCD_TRACE_1_COLOR);
    lora_cmp_button(LORA_CMP_EXIT_X, "EXIT", LCD_LOW_BAT_COLOR);
  } else
    lora_big(-1, 150, "Storing A...", 4, LCD_TRACE_1_COLOR);
}

static void lora_cmp_draw(void) {
  char buf[64];
  if (lora_cmp.state == LORA_CMP_ASK_A) {          // live level, helps to check the setup
    for (int i = 0; i < LORA_CMP_POINTS; i++) lora_cmp.b[i] = lora_s21_db(i);
    float l = lora_cmp_band_avg(lora_cmp.b);
    lcd_set_colors(LCD_FG_COLOR, LCD_BG_COLOR);
    lcd_fill(0, 200, LCD_WIDTH, 2 * FONT_GET_HEIGHT + FONT_STR_HEIGHT + 4);
    plot_printf(buf, sizeof(buf), "in-band S21 now: %.1f dB", l);
    lora_big(-1, 200, buf, 2, l < -70.0f ? LCD_LOW_BAT_COLOR : LCD_FG_COLOR);
    if (l < -70.0f) lora_big(-1, 200 + 2 * FONT_GET_HEIGHT + 2, "very low: check the port 2 / port 1 antennas and cables", 1, LCD_LOW_BAT_COLOR);
    return;
  }
  if (lora_cmp.state == LORA_CMP_STORING) {
    for (int i = 0; i < LORA_CMP_POINTS; i++) lora_cmp.a[i]+= lora_s21_db(i);
    if (++lora_cmp.count >= LORA_CMP_STORE_N) {
      for (int i = 0; i < LORA_CMP_POINTS; i++) lora_cmp.a[i]/= LORA_CMP_STORE_N;
      lora_cmp.a_valid = true; lora_cmp.a_f0 = frequency0; lora_cmp.a_f1 = frequency1;
      lora_cmp.state = LORA_CMP_LIVE; lora_cmp.b_init = false;
      lora_cmp_static();
    }
    return;
  }
  // LIVE: smoothed B
  for (int i = 0; i < LORA_CMP_POINTS; i++) {
    float v = lora_s21_db(i);
    lora_cmp.b[i] = lora_cmp.b_init ? 0.6f * lora_cmp.b[i] + 0.4f * v : v;
  }
  lora_cmp.b_init = true;
  lora_cmp_res_t r = lora_cmp_calc();
  // Big delta
  lcd_set_colors(LCD_FG_COLOR, LCD_BG_COLOR);
  lcd_fill(0, 56, LCD_WIDTH, 6 * FONT_GET_HEIGHT + 2 * FONT_GET_HEIGHT + 8);
  uint16_t c = r.d > 0.5f ? LCD_NORMAL_BAT_COLOR : r.d < -0.5f ? LCD_LOW_BAT_COLOR : LCD_TRACE_1_COLOR;
  plot_printf(buf, sizeof(buf), "%+.1f dB", r.d);
  lora_big(-1, 56, buf, 6, c);
  lora_big(-1, 56 + 6 * FONT_GET_HEIGHT + 4, r.d > 0.5f ? "B better than A" : r.d < -0.5f ? "B worse than A" : "B same as A (0.5 dB)", 2, c);
  // Levels and in-band spread
  int y = 56 + 8 * FONT_GET_HEIGHT + 12;
  lcd_set_colors(LCD_FG_COLOR, LCD_BG_COLOR);
  lcd_fill(0, y, LCD_WIDTH, FONT_STR_HEIGHT);
  plot_printf(buf, sizeof(buf), "A %.1f dB   B %.1f dB   in band: worst %+.1f  best %+.1f dB", r.a, r.b, r.worst, r.best);
  lora_big(-1, y, buf, 1, LCD_FG_COLOR);
  // Delta trace: B - A per point, +-6 dB, band shaded
  const int sx = 10, sw = LCD_WIDTH - 20, sy = y + FONT_STR_HEIGHT + 6, sh = LORA_CMP_BTN_Y - 8 - sy;
  lcd_set_background(LCD_BG_COLOR);
  lcd_fill(sx, sy, sw, sh);
  float k = (float)sw / (float)(frequency1 - frequency0);
  int bx0 = sx + ((float)lora_cmp.p.band_lo - (float)frequency0) * k;
  int bx1 = sx + ((float)lora_cmp.p.band_hi - (float)frequency0) * k;
  if (bx1 > bx0) {background_color = LORA_SHADE_COLOR; lcd_fill(bx0, sy, bx1 - bx0 + 1, sh);}
  lcd_set_foreground(LCD_GRID_COLOR);
  lcd_line(sx, sy, sx + sw, sy); lcd_line(sx, sy + sh, sx + sw, sy + sh);
  for (int xx = sx; xx < sx + sw; xx+= 8) lcd_line(xx, sy + sh / 2, xx + 3, sy + sh / 2);
  lcd_set_foreground(LCD_TRACE_2_COLOR);
  int px = 0, py = 0;
  for (int i = 0; i < LORA_CMP_POINTS && i < sweep_points; i++) {
    float d = lora_cmp.b[i] - lora_cmp.a[i];
    if (d > 6.0f) d = 6.0f; else if (d < -6.0f) d = -6.0f;
    int yy = sy + sh / 2 - (int)(d * (sh / 12.0f));
    int xx = sx + i * sw / (sweep_points - 1);
    if (i) {lcd_line(px, py, xx, yy); lcd_line(px, py + 1, xx, yy + 1);}
    px = xx; py = yy;
  }
  lcd_set_colors(LCD_GRID_COLOR, LCD_BG_COLOR);
  lcd_drawstring(sx + 2, sy + 2, "B-A +6 dB");
  lcd_drawstring(sx + 2, sy + sh - FONT_STR_HEIGHT, "-6 dB");
}

// draw_all() hook: full screen modes draw instead of the plot; also toast expiry
bool lora_fullscreen_draw(void) {
  if (lora_toast_s.on && chVTTimeElapsedSinceX(lora_toast_s.start) > MS2ST(1000)) {
    lora_toast_s.on = false;
    lora_toast_invalidate();
    request_to_redraw(REDRAW_CELLS);
  }
  if (lora_cmp.state != LORA_CMP_OFF) {
    if (redraw_request & REDRAW_CLRSCR) lora_cmp_static();
    if (redraw_request & REDRAW_PLOT) {
      if (lora_cmp.skip) lora_cmp.skip--;
      else lora_cmp_draw();
    }
    return true;
  }
  if (!lora_tune_s.on) return false;
  if (redraw_request & REDRAW_CLRSCR) lora_tune_static();
  if (redraw_request & REDRAW_PLOT) {
    if (lora_tune_s.skip) lora_tune_s.skip--;
    else lora_tune_draw();
  }
  return true;
}

//=====================================================================================================
// Shell: profile info / ref / tune lines
//=====================================================================================================
int shell_printf(const char *fmt, ...);
void lora_info_print(uint8_t mask) {
  const lora_measure_t *m = lora_meas;
  if (mask & LORA_PRINT_INFO) {
  shell_printf("res %u %.3f %.2f %.3f %.2f %.2f\r\n", (uint32_t)m->f_res, m->swr, m->rl, m->ml, m->r, m->x);
  bool bw_ok = m->bw && !(m->bw & 6);
  shell_printf("bw %u %d\r\n", m->bw ? (uint32_t)(m->f_hi - m->f_lo) : 0, (m->bw & 6) ? 1 : 0);
  shell_printf("q %.1f\r\n", bw_ok ? m->f_res / (m->f_hi - m->f_lo) : 0.0f);
  shell_printf("worst %.3f\r\n", m->worst);
  shell_printf("flag %s\r\n", lora_flag_names[m->flag]);
  for (int k = 0; k < m->ndip; k++)
    shell_printf("dip %u %.3f %u\r\n", (uint32_t)m->dip_f[k], m->dip_swr[k], lora_near_band(m->dip_f[k]));
  }
  if (mask & LORA_PRINT_REF)
  shell_printf("ref %d %u %.3f %u\r\n", lora_ref.valid ? 1 : 0, (uint32_t)lora_ref.f_res, lora_ref.swr, (uint32_t)lora_ref.bw);
  if (mask & LORA_PRINT_HOLD)
    shell_printf("hold %d %u %.3f %u\r\n", lora_hold.on ? 1 : 0, lora_hold.sweeps,
                 lora_hold.best_f > 0.0f ? lora_hold.best_swr : 0.0f, (uint32_t)lora_hold.best_f);
  if (mask & LORA_PRINT_HOLD_DATA)
    for (int i = 0; lora_hold.on && i < lora_hold.points && lora_hold.points == sweep_points; i++) {
      float gmin = vna_sqrtf(lora_hold.qmin[i] / 65535.0f), gmax = vna_sqrtf(lora_hold.qmax[i] / 65535.0f);
      shell_printf("%u %.3f %.3f\r\n", getFrequency(i), gmin < 0.999f ? (1 + gmin) / (1 - gmin) : 999.0f,
                   gmax < 0.999f ? (1 + gmax) / (1 - gmax) : 999.0f);
    }
  if (mask & LORA_PRINT_CMP) {
    lora_cmp_res_t r = lora_cmp_calc();
    shell_printf("compare %d %d %.2f %.2f %.2f %.2f %.2f\r\n", lora_cmp.state, lora_cmp.a_valid ? 1 : 0,
                 r.a, r.b, r.d, r.ok ? r.worst : 0.0f, r.ok ? r.best : 0.0f);
  }
  if (!(mask & LORA_PRINT_TUNE)) return;
  if (lora_tune_s.on && lora_tune_s.skip)
    shell_printf("tune 1 %u 0 0 0\r\n", lora_tune_s.p.target);
  else if (lora_tune_s.on) {
    lora_tune_res_t r = lora_tune_calc();
    float ft = (float)lora_tune_s.p.target;
    shell_printf("tune 1 %u %u %.3f %.2f\r\n", lora_tune_s.p.target, (uint32_t)r.f_res, r.swr_t, (r.f_res - ft) / ft * 100.0f);
  } else
    shell_printf("tune 0\r\n");
}

// Info block corner score: trace points (and marker plates, heavy) under the block
static int lora_box_score(int bx, int by, int bh) {
  int n = 0, x0 = bx - 2, x1 = bx + LORA_BOX_W + 2, y0 = by - 2, y1 = by + bh + 2;
  for (int t = 0; t < TRACES_MAX; t++) {
    if (!trace[t].enabled) continue;
    const index_t *idx = trace_index[t];
    for (int i = 0; i < sweep_points; i++)
      if (idx[i].x >= x0 && idx[i].x <= x1 && idx[i].y >= y0 && idx[i].y <= y1) n++;
    if (t != 0) continue;                        // marker plates on the SWR trace
    for (int m = 0; m < MARKERS_MAX; m++) {
      if (!markers[m].enabled) continue;
      const index_t *p = &idx[markers[m].index];
      if (p->x + MARKER_WIDTH >= x0 && p->x - MARKER_WIDTH <= x1 && p->y + 2*MARKER_HEIGHT >= y0 && p->y - 2*MARKER_HEIGHT <= y1) n+= 200;
    }
  }
  return n;
}

// Info block moved by hand: position for this session (x/2, y/2), x 0 = auto placement
// (the spare config bytes hold the firmware build stamp for the first boot chooser)
static uint8_t lora_box_cfg_x, lora_box_cfg_y;
bool lora_info_moved(void) {return lora_box_cfg_x != 0;}

static void lora_box_clamp(int *x, int *y) {
  if (*x > CELLOFFSETX + WIDTH - LORA_BOX_W) *x = CELLOFFSETX + WIDTH - LORA_BOX_W;
  if (*x < CELLOFFSETX + 1)                  *x = CELLOFFSETX + 1;
  if (*y > HEIGHT - lp.box_h)                *y = HEIGHT - lp.box_h;
  if (*y < 0)                                *y = 0;
}

static void lora_box_rect(int corner, int *x, int *y) {
  int right = CELLOFFSETX + WIDTH - LORA_BOX_W - 2;
#ifdef __USE_GRID_VALUES__
  if (VNA_MODE(VNA_MODE_SHOW_GRID)) right = GRID_X_TEXT - LORA_BOX_W - 2;
#endif
  *x = (corner & 1) ? right : CELLOFFSETX + 2;
  *y = (corner & 2) ? HEIGHT - lp.box_h - 2 : LORA_LBL_Y + LORA_LBL_H;
}

static void lora_invalidate_box(void) {
  invalidate_rect(lp.box_x, lp.box_y, lp.box_x + LORA_BOX_W, lp.box_y + lp.box_h);
}

// Set box position from the corner (auto) or the config bytes (manual), invalidate old/new place
static void lora_box_position(void) {
  int bx, by;
  if (lora_box_cfg_x) {bx = lora_box_cfg_x * 2; by = lora_box_cfg_y * 2; lora_box_clamp(&bx, &by);}
  else                lora_box_rect(lp.corner, &bx, &by);
  if (bx != lp.box_x || by != lp.box_y) {lora_invalidate_box(); lp.box_x = bx; lp.box_y = by;}
  lora_invalidate_box();
}

// ui.c side: info block rect in area coordinates, false if not shown
bool lora_info_rect(int *x, int *y, int *w, int *h) {
  if (!lora_screen_on || lp.box_h == 0) return false;
  *x = lp.box_x; *y = lp.box_y; *w = LORA_BOX_W; *h = lp.box_h;
  return true;
}

// ui.c side: move the info block by hand (area coordinates, clamped), x < 0 = back to auto placement
void lora_info_place(int x, int y) {
  if (x < 0) lora_box_cfg_x = 0;
  else {
    lora_box_clamp(&x, &y);
    lora_box_cfg_x = x / 2; lora_box_cfg_y = y / 2;
  }
  lora_box_position();
}

static void prepare_lora_info(uint8_t type, uint8_t update_mask) {
  (void)type;
  lora_measure_t *m = lora_meas;
  if (update_mask & MEASURE_UPD_SWEEP) {
    int n = sweep_points, i, x = lora_min_index();
    float y = lora_g2(x);
    m->res_idx = x;
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
    // Worst SWR inside the band (band profiles)
    m->worst = -1.0f;
    if (lora_screen_on && lp.s.nband == 1)
      for (i = 0; i < n; i++) {
        freq_t f = getFrequency(i);
        if (f < lp.s.band[0][0] || f > lp.s.band[0][1]) continue;
        float v = swr(i, measured[0][i]);
        if (!(v <= m->worst)) m->worst = v;     // NaN/INF -> worst
      }
    m->ml = lora_ml(y);
    lora_sanity(m);
    lora_dips(m);
  }
  if (!lora_screen_on) {
    invalidate_rect(STR_MEASURE_X                        , STR_MEASURE_Y,
                    STR_MEASURE_X + 5 * STR_MEASURE_WIDTH, STR_MEASURE_Y + 5 * STR_MEASURE_HEIGHT);
    return;
  }
  // Band marker labels (M2..): SWR at the marker, at the top of the plot, 2 rows if overlapping
  int prev_end = -1000, prev_row = 1;
  for (int k = 0; k < LORA_MK_MAX; k++) {
    int mk = k + 1;
    lp.lbl_swr[k] = -1.0f;
    if (!markers[mk].enabled) continue;
    int idx = markers[mk].index;
    float v = swr(idx, measured[0][idx]);
    char sv[8];
    lora_fmt_swr(sv, sizeof(sv), v);
    if (lp.s.mk) plot_printf(lp.lbl[k], sizeof(lp.lbl[k]), "%u:%s", (uint32_t)(lp.s.mk[k] / 1000000U), sv);
    else         plot_printf(lp.lbl[k], sizeof(lp.lbl[k]), "%c %s", k == 0 ? 'L' : 'H', sv);
    int w  = lora_str_width(lp.lbl[k]);
    int xm = CELLOFFSETX + (idx * WIDTH + (sweep_points - 1) / 2) / (sweep_points - 1);
    int x  = lp.s.mk ? xm - w / 2 : (k == 0 ? xm - 3 - w : xm + 3);   // band: lo left of edge, hi right
    if (x < CELLOFFSETX + 1) x = CELLOFFSETX + 1;
    if (x + w > CELLOFFSETX + WIDTH) x = CELLOFFSETX + WIDTH - w;
    int row = (x < prev_end + FONT_WIDTH) ? (prev_row ^ 1) : 0;
    lp.lbl_x[k] = x; lp.lbl_row[k] = row; lp.lbl_swr[k] = v;
    prev_end = x + w; prev_row = row;
  }
  invalidate_rect(0, 0, AREA_WIDTH_NORMAL - 1, LORA_LBL_Y + LORA_LBL_H);
  // Info block: line count, then the plot corner with the least trace points under it
  int lines = 6;                                     // chips, M1/res, swr/rl, z, bw, legend
  if (!(props_mode & TD_MARKER_TRACK)) lines++;      // M1 FREE: M1 readout + resonance line
  if (get_electrical_delay() != 0.0f) lines++;
  if (lp.s.nband > 1 && m->ndip > 1) lines+= m->ndip - 1; // WIDE/SURVEY: dips instead of the BW line
  if (lora_ref_here()) lines++;                      // vs REF
  if (lora_hold.on) lines++;                         // HOLD best
  int bh = lines * LORA_LINE_H + 3;
  if (bh != lp.box_h || lines != lp.lines) {lora_invalidate_box(); lp.box_h = bh; lp.lines = lines;}
  if ((update_mask & MEASURE_UPD_SWEEP) && !lora_box_cfg_x) {
    int score[4], best = lp.corner, bx, by;
    for (int c = 0; c < 4; c++) {
      lora_box_rect(c, &bx, &by);
      score[c] = lora_box_score(bx, by, bh);
      if (score[c] < score[best]) best = c;
    }
    if (best != lp.corner && score[best] + 6 < score[lp.corner]) {   // hysteresis: no flicker
      lora_invalidate_box();
      lp.corner = best;
    }
  }
  lora_box_position();
}

// Filled chip with black text (no shadow), return width
static int lora_chip(int x, int y, const char *s, pixel_t bg) {
  int w = lora_str_width(s) + 6;
  lora_cell_fill(x, y - 1, w, FONT_STR_HEIGHT + 1, bg);
  lcd_set_foreground(LCD_BG_COLOR);
  cell_text_shadow = false;
  cell_printf(x + 3, y, "%s", s);
  cell_text_shadow = true;
  return w;
}

// Legacy layout (LORA INFO selected outside a profile range): text at the stock measure position
static void draw_lora_info_text(int xp, int yp) {
  const lora_measure_t *m = lora_meas;
  cell_printf(xp, yp, "Res %.6F" S_Hz "  SWR %.2f  RL %.1f" S_dB, m->f_res, m->swr, m->rl);
  yp += STR_MEASURE_HEIGHT;
  cell_printf(xp, yp, "Z %.1f%+j.1f" S_OHM, m->r, m->x);
  if (m->bw)
    cell_printf(xp + 2 * STR_MEASURE_WIDTH, yp, "BW(SWR<2) %s%.3F" S_Hz, (m->bw & 6) ? ">" : "", m->f_hi - m->f_lo);
  else
    cell_printf(xp + 2 * STR_MEASURE_WIDTH, yp, "BW(SWR<2) none");
}

static void draw_lora_info(int xp, int yp) {
  if (!lora_screen_on) {draw_lora_info_text(xp, yp); return;}
  const lora_measure_t *m = lora_meas;
  int x0 = STR_MEASURE_X - xp, y0 = STR_MEASURE_Y - yp;   // cell origin
  char buf[24];
  // Band marker labels at the top
  if (y0 < LORA_LBL_Y + LORA_LBL_H)
    for (int k = 0; k < LORA_MK_MAX; k++) {
      if (lp.lbl_swr[k] < 0.0f) continue;
      foreground_color = lora_swr_color(lp.lbl_swr[k]);
      cell_printf(lp.lbl_x[k] - x0, LORA_LBL_Y + lp.lbl_row[k] * FONT_STR_HEIGHT - y0, "%s", lp.lbl[k]);
    }
  // Info block (skip cells outside), toast on top
  int bx = lp.box_x - x0, by = lp.box_y - y0;
  if (bx >= CELLWIDTH || bx + LORA_BOX_W <= 0 || by >= CELLHEIGHT || by + lp.box_h <= 0) {lora_cell_toast(x0, y0); return;}
  lora_cell_fill(bx, by, LORA_BOX_W, lp.box_h, GET_PALETTE_COLOR(LCD_BG_COLOR));
  pixel_t frame = GET_PALETTE_COLOR(LCD_GRID_COLOR);
  lora_cell_fill(bx, by, LORA_BOX_W, 1, frame);
  lora_cell_fill(bx, by + lp.box_h - 1, LORA_BOX_W, 1, frame);
  lora_cell_fill(bx, by, 1, lp.box_h, frame);
  lora_cell_fill(bx + LORA_BOX_W - 1, by, 1, lp.box_h, frame);
  int x = bx + 3, y = by + 2;
  // 1: title, pass/fail chip (worst SWR in band), cal chip
  lcd_set_foreground(LCD_FG_COLOR);
  x+= lora_print(x, y, lp.s.title) + 4;
  if (m->flag != LORA_FLAG_OK) {                     // antenna sanity first
    static const char *ft[] = {"", "NO ANT/OPEN?", "SHORT?", "LOSSY?"};
    x+= lora_chip(x, y, ft[m->flag], GET_PALETTE_COLOR(m->flag == LORA_FLAG_LOSSY ? LCD_TRACE_1_COLOR : LCD_LOW_BAT_COLOR)) + 3;
  } else if (m->worst >= 0.0f && lp.s.cal != LORA_CAL_NONE) {
    char sv[8];
    lora_fmt_swr(sv, sizeof(sv), m->worst);
    plot_printf(buf, sizeof(buf), "%s worst %s", m->worst <= 1.5f ? "OK" : m->worst <= 2.0f ? "~OK" : "BAD", sv);
    x+= lora_chip(x, y, buf, lora_swr_color(m->worst)) + 3;
  }
  static const struct {const char *text; uint8_t color;} cal_chip[] = {
    [LORA_CAL_OK]    = {"CAL OK",    LCD_NORMAL_BAT_COLOR},
    [LORA_CAL_RANGE] = {"CAL RANGE", LCD_TRACE_1_COLOR},
    [LORA_CAL_NONE]  = {"NO CAL",    LCD_LOW_BAT_COLOR},
  };
  lora_chip(x, y, cal_chip[lp.s.cal].text, GET_PALETTE_COLOR(cal_chip[lp.s.cal].color));
  x = bx + 3; y+= LORA_LINE_H;
  lcd_set_foreground(LCD_MEASURE_COLOR);
  bool track = props_mode & TD_MARKER_TRACK;
  int i = markers[0].enabled ? markers[0].index : m->res_idx;
  // 2: M1 state + frequency. TRACK: M1 = resonance (interpolated) + offset from band centre,
  //    FREE: M1 frequency (moved by jog/touch, long jog press = back to TRACK)
  foreground_color = GET_PALETTE_COLOR(track ? LCD_TRACE_1_COLOR : LCD_LOW_BAT_COLOR);
  int w = lora_print(x, y, track ? "M1 TRACK " : "M1 FREE ");
  lcd_set_foreground(LCD_FG_COLOR);
  if (!track)
    cell_printf(x + w, y, "%.6F" S_Hz, (float)getFrequency(i));
  else if (lp.s.target)
    cell_printf(x + w, y, "%.6F" S_Hz " %+.3F" S_Hz, m->f_res, m->f_res - (float)lp.s.target);
  else
    cell_printf(x + w, y, "%.6F" S_Hz, m->f_res);
  y+= LORA_LINE_H;
  // 3: SWR (colour = quality) and return loss at M1 (TRACK: at the minimum)
  float v_swr = track ? m->swr : swr(i, measured[0][i]);
  float v_rl  = track ? m->rl  : -logmag(i, measured[0][i]);
  lora_fmt_swr(buf, sizeof(buf), v_swr);
  w = lora_print(x, y, "SWR ");
  foreground_color = lora_swr_color(v_swr);
  w+= lora_print(x + w, y, buf);
  lcd_set_foreground(LCD_FG_COLOR);
  cell_printf(x + w, y, "  RL %.1f" S_dB "  ML %.2f" S_dB, v_rl, track ? m->ml : lora_ml(lora_g2(i)));
  y+= LORA_LINE_H;
  // 4: impedance at M1
  if (track) cell_printf(x, y, "Z %.1f%+j.1f" S_OHM, m->r, m->x);
  else       cell_printf(x, y, "Z %.1f%+j.1f" S_OHM, resistance(i, measured[0][i]), reactance(i, measured[0][i]));
  y+= LORA_LINE_H;
  lcd_set_foreground(LCD_MEASURE_COLOR);
  // FREE: resonance summary stays visible
  if (!track) {
    lora_fmt_swr(buf, sizeof(buf), m->swr);
    if (lp.s.target)
      cell_printf(x, y, "Res %.5F" S_Hz " %+.3F" S_Hz, m->f_res, m->f_res - (float)lp.s.target);
    else
      cell_printf(x, y, "Res %.5F" S_Hz " SWR %s", m->f_res, buf);
    y+= LORA_LINE_H;
  }
  // 5: WIDE/SURVEY: dips with the nearest LoRa band, else SWR<=2 bandwidth and Q = f/BW
  if (lp.s.nband > 1) {
    if (!m->ndip) {cell_printf(x, y, "No dip below SWR 3"); y+= LORA_LINE_H;}
    for (int k = 0; k < m->ndip; k++, y+= LORA_LINE_H) {
      lora_fmt_swr(buf, sizeof(buf), m->dip_swr[k]);
      uint32_t b = lora_near_band(m->dip_f[k]);
      cell_printf(x, y, "Dip %.5F" S_Hz " %s (%u%+.3F" S_Hz ")", m->dip_f[k], buf, b, m->dip_f[k] - b * 1e6f);
    }
  } else {
  if (!m->bw)
    cell_printf(x, y, "BW(SWR<2) none");
  else if (m->bw & 6)
    cell_printf(x, y, "BW(SWR<2) >%.3F" S_Hz, m->f_hi - m->f_lo);
  else
    cell_printf(x, y, "BW(SWR<2) %.3F" S_Hz "  Q %.1f", m->f_hi - m->f_lo, m->f_res / (m->f_hi - m->f_lo));
  y+= LORA_LINE_H;
  }
  // vs REF: resonance shift and SWR change since REF
  if (lora_ref_here()) {
    lcd_set_foreground(LCD_TRACE_2_COLOR);
    cell_printf(x, y, "vs REF " S_DELTA "f %+.3F" S_Hz " " S_DELTA "SWR %+.2f", m->f_res - lora_ref.f_res, m->swr - lora_ref.swr);
    lcd_set_foreground(LCD_MEASURE_COLOR);
    y+= LORA_LINE_H;
  }
  if (lora_hold.on) {
    lcd_set_foreground(LCD_TRACE_1_COLOR);
    if (lora_hold.best_f > 0.0f) {
      lora_fmt_swr(buf, sizeof(buf), lora_hold.best_swr);
      cell_printf(x, y, "HOLD best SWR %s @ %.5F" S_Hz, buf, lora_hold.best_f);
    } else
      cell_printf(x, y, "HOLD on, waiting for a sweep");
    lcd_set_foreground(LCD_MEASURE_COLOR);
    y+= LORA_LINE_H;
  }
  float ed = get_electrical_delay();
  if (ed != 0.0f) {
    cell_printf(x, y, "Edelay %F" S_SECOND, ed);
    y+= LORA_LINE_H;
  }
  // Legend: trace colour + scale per division
  for (int t = 0; t < TRACES_MAX; t++) {
    if (!trace[t].enabled) continue;
    float sc = get_trace_scale(t);
    switch (trace[t].type) {
      case TRC_SWR:    {float r = get_trace_refpos(t);  // SWR: bottom..top of the plot
                        plot_printf(buf, sizeof(buf), "SWR %F-%F", 1.0f - sc * r, 1.0f + sc * (NGRIDY - r)); break;}
      case TRC_LOGMAG: plot_printf(buf, sizeof(buf), "RL %F" S_dB "/", sc); break;
      case TRC_X:      plot_printf(buf, sizeof(buf), "X %F" S_OHM "/", sc); break;
      case TRC_SMITH:  plot_printf(buf, sizeof(buf), "Smith"); break;
      default:         plot_printf(buf, sizeof(buf), "%s", get_trace_typename(trace[t].type, trace[t].smith_format)); break;
    }
    lcd_set_foreground(LCD_TRACE_1_COLOR + t);
    x+= lora_print(x, y, buf) + FONT_WIDTH;
  }
  lora_cell_toast(x0, y0);
}
#endif // __VNA_LORA_PROFILES__

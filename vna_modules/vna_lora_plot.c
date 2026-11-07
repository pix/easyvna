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
  uint16_t res_idx;   // point of the minimum (M1 tracking)
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

// Info block moved by hand: position in spare config bytes (x/2, y/2), x byte 0 = auto placement
#define lora_box_cfg_x   config._reserved[1]
#define lora_box_cfg_y   config._reserved[2]

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

// ui.c side: move the info block by hand (area coordinates, clamped), x < 0 = back to auto placement.
// Only RAM config is changed here, the caller saves it.
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
  // Info block (skip cells outside)
  int bx = lp.box_x - x0, by = lp.box_y - y0;
  if (bx >= CELLWIDTH || bx + LORA_BOX_W <= 0 || by >= CELLHEIGHT || by + lp.box_h <= 0) return;
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
  if (m->worst >= 0.0f && lp.s.cal != LORA_CAL_NONE) {
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
  cell_printf(x + w, y, "  RL %.1f" S_dB, v_rl);
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
  // 5: SWR<=2 bandwidth and Q = f/BW
  if (!m->bw)
    cell_printf(x, y, "BW(SWR<2) none");
  else if (m->bw & 6)
    cell_printf(x, y, "BW(SWR<2) >%.3F" S_Hz, m->f_hi - m->f_lo);
  else
    cell_printf(x, y, "BW(SWR<2) %.3F" S_Hz "  Q %.1f", m->f_hi - m->f_lo, m->f_res / (m->f_hi - m->f_lo));
  y+= LORA_LINE_H;
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
}
#endif // __VNA_LORA_PROFILES__

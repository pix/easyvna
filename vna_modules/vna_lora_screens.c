/*
 * LoRa profiles: full screen / action bar drawing (lcd_* API only, no UI state).
 * Included from vna_profiles.c (ui.c); the logic (input, actions) is there.
 *  - action bar: 6 big buttons in place of the frequency line on the profile screen
 *  - profile chooser: boot row on the splash, or full screen grid (from the action bar)
 *  - QUICK CAL wizard screens
 *
 * This is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3, or (at your option) any later version.
 */
#ifdef __VNA_LORA_PROFILES__

// Action bar geometry (below the plot)
#define LORA_BAR_Y      (AREA_HEIGHT_NORMAL + OFFSETY + 1)
#define LORA_BAR_H      (LCD_HEIGHT - LORA_BAR_Y)
#define LORA_BAR_N      6
#define LORA_BAR_W      (LCD_WIDTH / LORA_BAR_N)

// Text width in pixels with font scale
static int lora_scr_text_w(const char *s, int size) {
  int w = 0;
  for (; *s; s++) if ((uint8_t)*s >= FONT_START_CHAR) w+= FONT_GET_WIDTH((uint8_t)*s);
  return w * size;
}

// Text centred in [x, x + w), colours: fg palette index on raw background pixel
static void lora_scr_center(int x, int y, int w, const char *s, int size, uint16_t fg, pixel_t bg) {
  lcd_set_foreground(fg);
  background_color = bg;
  lcd_drawstring_size(s, x + (w - lora_scr_text_w(s, size)) / 2, y, size);
}

// Flat button: 1 px frame, fill, up to 3 centred lines (line 1 scaled by size1)
static void lora_scr_button(int x, int y, int w, int h, pixel_t bg, uint16_t fg, bool sel,
                            const char *l1, int size1, const char *l2, uint16_t fg2, const char *l3, uint16_t fg3) {
  background_color = GET_PALETTE_COLOR(sel ? LCD_FG_COLOR : LCD_GRID_COLOR);
  lcd_fill(x, y, w, h);
  int b = sel ? 3 : 1;
  background_color = bg;
  lcd_fill(x + b, y + b, w - 2 * b, h - 2 * b);
  int lines_h = FONT_GET_HEIGHT * size1 + (l2 ? FONT_STR_HEIGHT + 2 : 0) + (l3 ? FONT_STR_HEIGHT + 2 : 0);
  int ty = y + (h - lines_h) / 2;
  lora_scr_center(x, ty, w, l1, size1, fg, bg);
  ty+= FONT_GET_HEIGHT * size1 + 2;
  if (l2) {lora_scr_center(x, ty, w, l2, 1, fg2, bg); ty+= FONT_STR_HEIGHT + 2;}
  if (l3) lora_scr_center(x, ty, w, l3, 1, fg3, bg);
}

// Action bar button i: 2 lines (name, state)
static void lora_scr_bar_button(int i, const char *name, const char *state, pixel_t bg, uint16_t fg) {
  lora_scr_button(i * LORA_BAR_W, LORA_BAR_Y, (i == LORA_BAR_N - 1) ? LCD_WIDTH - i * LORA_BAR_W : LORA_BAR_W,
                  LORA_BAR_H, bg, fg, false, name, 1, state, fg, NULL, fg);
}

//=====================================================================================================
// Profile chooser: layout 0 = boot row (on the splash), 1 = full screen grid 3x2 + CANCEL
//=====================================================================================================
#define LORA_CH_N       6
// boot row
#define BOOT_BTN_H      (FONT_STR_HEIGHT * 2 + 14)
#define BOOT_BTN_GAP    6
#define BOOT_BTN_Y      (LCD_HEIGHT - 6 - 2 * (FONT_STR_HEIGHT + 2) - 6 - BOOT_BTN_H)
#define BOOT_TEXT_Y     (BOOT_BTN_Y - FONT_STR_HEIGHT - 6)    // top of chooser area (splash keeps above)
#define BOOT_BTN_W      ((LCD_WIDTH - 2 * 8 - (LORA_CH_N - 1) * BOOT_BTN_GAP) / LORA_CH_N)
// grid
#define LORA_GRID_W     150
#define LORA_GRID_H     96
#define LORA_GRID_GAP   8
#define LORA_GRID_X     ((LCD_WIDTH - 3 * LORA_GRID_W - 2 * LORA_GRID_GAP) / 2)
#define LORA_GRID_Y     58
#define LORA_CANCEL_W   200
#define LORA_CANCEL_H   40
#define LORA_CANCEL_X   ((LCD_WIDTH - LORA_CANCEL_W) / 2)
#define LORA_CANCEL_Y   (LCD_HEIGHT - LORA_CANCEL_H - 8)

typedef struct {
  const char *name;     // big name
  const char *range;    // range text (grid only)
  const char *tag;      // cal state text
  uint16_t    tag_fg;   // cal state colour (palette index)
} lora_choice_t;

static void lora_scr_choice_rect(int layout, int d, int *x, int *y, int *w, int *h) {
  if (layout == 0) {
    *x = 8 + d * (BOOT_BTN_W + BOOT_BTN_GAP); *y = BOOT_BTN_Y; *w = BOOT_BTN_W; *h = BOOT_BTN_H;
  } else {
    *x = LORA_GRID_X + (d % 3) * (LORA_GRID_W + LORA_GRID_GAP);
    *y = LORA_GRID_Y + (d / 3) * (LORA_GRID_H + LORA_GRID_GAP);
    *w = LORA_GRID_W; *h = LORA_GRID_H;
  }
}

// Display index under the touch point, -1 = none (grid: -2 = CANCEL)
static int lora_scr_choice_hit(int layout, int tx, int ty) {
  for (int d = 0; d < LORA_CH_N; d++) {
    int x, y, w, h;
    lora_scr_choice_rect(layout, d, &x, &y, &w, &h);
    if (tx >= x && tx < x + w && ty >= y && ty < y + h) return d;
  }
  if (layout && tx >= LORA_CANCEL_X && tx < LORA_CANCEL_X + LORA_CANCEL_W && ty >= LORA_CANCEL_Y) return -2;
  return -1;
}

static void lora_scr_choice_button(int layout, int d, const lora_choice_t *c, bool sel) {
  int x, y, w, h;
  lora_scr_choice_rect(layout, d, &x, &y, &w, &h);
  pixel_t bg = GET_PALETTE_COLOR(sel ? LCD_TRACE_1_COLOR : LCD_MENU_COLOR);
  uint16_t tag_fg = (sel && c->tag_fg == LCD_NORMAL_BAT_COLOR) ? LCD_MENU_TEXT_COLOR : c->tag_fg;
  if (layout == 0)
    lora_scr_button(x, y, w, h, bg, LCD_MENU_TEXT_COLOR, sel, c->name, 1, c->tag, tag_fg, NULL, 0);
  else
    lora_scr_button(x, y, w, h, bg, LCD_MENU_TEXT_COLOR, sel, c->name, 3, c->range, LCD_MENU_TEXT_COLOR, c->tag, tag_fg);
}

// Full chooser: buttons + help text (secs: boot countdown, < 0 = wait, layout 1 ignores it)
static void lora_scr_chooser(int layout, const lora_choice_t *c, int sel_d, int secs, const char *sel_name) {
  if (layout == 1) {
    lcd_set_colors(LCD_FG_COLOR, LCD_BG_COLOR);
    lcd_clear_screen();
    lora_scr_center(0, 8, LCD_WIDTH, "CHOOSE A PROFILE", 2, LCD_FG_COLOR, GET_PALETTE_COLOR(LCD_BG_COLOR));
    lora_scr_center(0, 36, LCD_WIDTH, "tap a profile, or jog + push", 1, LCD_GRID_COLOR, GET_PALETTE_COLOR(LCD_BG_COLOR));
    lora_scr_button(LORA_CANCEL_X, LORA_CANCEL_Y, LORA_CANCEL_W, LORA_CANCEL_H, GET_PALETTE_COLOR(LCD_GRID_COLOR),
                    LCD_FG_COLOR, false, "CANCEL", 2, NULL, 0, NULL, 0);
  }
  for (int d = 0; d < LORA_CH_N; d++)
    lora_scr_choice_button(layout, d, &c[d], d == sel_d);
  if (layout == 0) {
    lcd_set_colors(LCD_FG_COLOR, LCD_BG_COLOR);
    lcd_fill(0, BOOT_TEXT_Y, LCD_WIDTH, FONT_STR_HEIGHT);
    if (secs < 0)
      lcd_printf(8, BOOT_TEXT_Y, "First start: choose a mode (jog + push, or touch)");
    else
      lcd_printf(8, BOOT_TEXT_Y, "Starting %s in %ds - jog/touch to choose", sel_name, secs);
  }
}

//=====================================================================================================
// QUICK CAL wizard screens
//=====================================================================================================
#define LORA_CAL_STEPS  3
#define LORA_CAL_CANCEL_X 8
#define LORA_CAL_CANCEL_Y (LCD_HEIGHT - 52)
#define LORA_CAL_CANCEL_W 150
#define LORA_CAL_CANCEL_H 44
static const char *lora_cal_names[LORA_CAL_STEPS] = {"OPEN", "SHORT", "LOAD"};

static bool lora_scr_cal_cancel_hit(int tx, int ty) {
  return tx < LORA_CAL_CANCEL_X + LORA_CAL_CANCEL_W && ty >= LORA_CAL_CANCEL_Y - 4;
}

// step: 0..2 current step, 3 = saved; state: 0 = connect, 1 = measuring, 2 = done (msg)
static void lora_scr_cal(const char *title, int slot, int step, int state, const char *msg) {
  char buf[40];
  pixel_t bg0 = GET_PALETTE_COLOR(LCD_BG_COLOR);
  lcd_set_colors(LCD_FG_COLOR, LCD_BG_COLOR);
  lcd_clear_screen();
  plot_printf(buf, sizeof(buf), "QUICK CAL  %s", title);
  lora_scr_center(0, 8, LCD_WIDTH, buf, 2, LCD_FG_COLOR, bg0);
  // Progress row
  for (int i = 0; i < LORA_CAL_STEPS; i++) {
    int w = 140, x = (LCD_WIDTH - 3 * w - 2 * 10) / 2 + i * (w + 10);
    pixel_t bg = GET_PALETTE_COLOR(i < step ? LCD_NORMAL_BAT_COLOR : i == step ? LCD_TRACE_1_COLOR : LCD_GRID_COLOR);
    plot_printf(buf, sizeof(buf), "%d %s%s", i + 1, lora_cal_names[i], i < step ? " OK" : "");
    lora_scr_button(x, 40, w, 34, bg, LCD_MENU_TEXT_COLOR, i == step && state != 2, buf, 1, NULL, 0, NULL, 0);
  }
  if (state == 2) {                                  // finished
    lora_scr_center(0, 110, LCD_WIDTH, msg, 4, LCD_NORMAL_BAT_COLOR, bg0);
    plot_printf(buf, sizeof(buf), "slot %d - tap or push to continue", slot);
    lora_scr_center(0, 180, LCD_WIDTH, buf, 1, LCD_FG_COLOR, bg0);
    return;
  }
  plot_printf(buf, sizeof(buf), "Connect %s", lora_cal_names[step]);
  lora_scr_center(0, 100, LCD_WIDTH, buf, 4, LCD_FG_COLOR, bg0);
  if (state == 1)
    lora_scr_center(0, 165, LCD_WIDTH, "Measuring...", 2, LCD_TRACE_1_COLOR, bg0);
  else {
    lora_scr_center(0, 165, LCD_WIDTH, "to the cable end, then TAP", 2, LCD_FG_COLOR, bg0);
    lora_scr_center(0, 195, LCD_WIDTH, "(or push the jog wheel)", 1, LCD_GRID_COLOR, bg0);
  }
  plot_printf(buf, sizeof(buf), "saves to slot %d", slot);
  lora_scr_center(LCD_WIDTH / 2, LCD_HEIGHT - 36, LCD_WIDTH / 2, buf, 1, LCD_GRID_COLOR, bg0);
  lora_scr_button(LORA_CAL_CANCEL_X, LORA_CAL_CANCEL_Y, LORA_CAL_CANCEL_W, LORA_CAL_CANCEL_H,
                  GET_PALETTE_COLOR(LCD_LOW_BAT_COLOR), LCD_FG_COLOR, false, "CANCEL", 2, NULL, 0, NULL, 0);
}
#endif // __VNA_LORA_PROFILES__

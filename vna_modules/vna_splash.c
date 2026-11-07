/*
 * Boot splash screen (EasyVNA by PiX), included from ui.c, enabled by __VNA_SPLASH__
 * Shown once after lcd_init(), before the sweep thread starts.
 *
 * Copyright (c) 2026, Camille Moncelier (PiX)
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without modification, are
 * permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice, this list of
 *    conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of
 *    conditions and the following disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to
 *    endorse or promote products derived from this software without specific prior written
 *    permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 * (Part of EasyVNA, a NanoVNA-D fork; the firmware as a whole is GPL-3.0, see LICENSE.)
 */
#ifdef __VNA_SPLASH__

#define SPLASH_TIME_MS 1500

// Centered string with scaled font
static void splash_text(int y, const char *s, uint8_t size, uint16_t color) {
  lcd_set_foreground(color);
  lcd_drawstring_size(s, (LCD_WIDTH - (int)strlen(s) * FONT_WIDTH * size) / 2, y, size);
}

void ui_splash(void) {
  // Called before ui_init()/sweep thread: apply flip and backlight here, else splash is dark (H4 DAC = 0)
#ifdef __FLIP_DISPLAY__
  if (VNA_MODE(VNA_MODE_FLIP_DISPLAY))
    lcd_set_flip(true);
#endif
#ifdef __LCD_BRIGHTNESS__
  lcd_setBrightness(config._brightness);
#endif
  lcd_set_colors(LCD_FG_COLOR, LCD_BG_COLOR);
  lcd_clear_screen();
  // Accent bars
  lcd_set_background(LCD_TRACE_2_COLOR);
  lcd_fill(0, 0, LCD_WIDTH, 4);
  lcd_fill(0, LCD_HEIGHT - 4, LCD_WIDTH, 4);
  lcd_set_background(LCD_BG_COLOR);

  int y = LCD_HEIGHT / 16;
  splash_text(y, BOARD_NAME, 2, LCD_FG_COLOR);
  y += FONT_GET_HEIGHT * 2 + LCD_HEIGHT / 32;
  splash_text(y, "EasyVNA", 4, LCD_TRACE_1_COLOR);
  y += FONT_GET_HEIGHT * 4 + LCD_HEIGHT / 32;
  splash_text(y, "by PiX", 2, LCD_TRACE_2_COLOR);
  y += FONT_GET_HEIGHT * 2 + 2;
  splash_text(y, "(and the Claude, OpenAI and Deepseek AI band)", 1, LCD_TRACE_2_COLOR);
  y += FONT_GET_HEIGHT + LCD_HEIGHT / 24;

  // Stylized S11 dip: grid line, band edges, SWR-like resonance curve
  const int x0 = LCD_WIDTH / 10, w = LCD_WIDTH - 2 * x0;
#ifdef __VNA_LORA_PROFILES__
  int h = BOOT_TEXT_Y - 6 - y;                                 // keep the chooser area free
#else
  int h = LCD_HEIGHT - 6 - 2 * (FONT_STR_HEIGHT + 2) - 10 - y;
#endif
  if (h > LCD_HEIGHT / 4) h = LCD_HEIGHT / 4;
  lcd_set_foreground(LCD_GRID_COLOR);
  for (int x = x0; x < x0 + w; x += 8)
    lcd_line(x, y + h * 2 / 3, x + 3, y + h * 2 / 3);         // dashed "SWR 2" line
  lcd_set_foreground(LCD_TRACE_3_COLOR);
  lcd_line(x0 + w * 44 / 100, y, x0 + w * 44 / 100, y + h);   // band edges
  lcd_line(x0 + w * 56 / 100, y, x0 + w * 56 / 100, y + h);
  lcd_set_foreground(LCD_TRACE_1_COLOR);
  int px = x0, py = y;
  for (int i = 1; i <= 64; i++) {
    float f = (i / 64.0f - 0.5f) * 9.0f;                       // normalized detuning
    int nx = x0 + w * i / 64;
    int ny = y + (int)(h / (1.0f + f * f));                    // dip goes down at resonance
    lcd_line(px, py, nx, ny);
    lcd_line(px, py + 1, nx, ny + 1);
    px = nx; py = ny;
  }

  // Version info at the bottom (power on profile chooser goes between graph and this)
  y = LCD_HEIGHT - 6 - 2 * (FONT_STR_HEIGHT + 2);
  lcd_set_foreground(LCD_GRID_COLOR);
  lcd_printf(x0, y, "EasyVNA %s  build " __DATE__, VERSION);
  lcd_printf(x0, y + FONT_STR_HEIGHT + 2, "based on NanoVNA-D by DiSlord (edy555, hugen79)");
#ifdef __VNA_LORA_PROFILES__
  uint32_t fpc;
  if (fault_log_present(&fpc)) {                 // crash in the previous run (shell: fault)
    lcd_set_foreground(LCD_LOW_BAT_COLOR);
    lcd_printf(x0, 8, "Previous run crashed at PC 0x%08x - shell: fault", fpc);
  }
#endif
  lcd_set_foreground(LCD_FG_COLOR);
#ifdef __VNA_LORA_PROFILES__
  if (lora_boot_asks() || lora_first_boot()) return;  // chooser (after ui_init) keeps the splash on screen
#endif
  chThdSleepMilliseconds(SPLASH_TIME_MS);
}
#endif // __VNA_SPLASH__

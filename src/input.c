#include "input.h"

#include <ncurses.h>

#include "fluid.h"

#define UI_GAIN_STEP 1.15f
#define UI_GAIN_MIN  0.35f
#define UI_GAIN_MAX  3.0f
#define UI_OVERLAY_SECONDS 9.0

/*
 * Discrete token-rate ladder: level 0 freezes the hexdump and any step up
 * from there works (a multiplicative rate could never climb out of zero).
 */
static const float ui_rate_levels[] = {
    0.0f, 0.010f, 0.030f, 0.060f, 0.10f, 0.17f, 0.28f, 0.45f, 0.72f, 1.0f
};
#define UI_RATE_LEVELS ((int)(sizeof(ui_rate_levels) / sizeof(ui_rate_levels[0])))
#define UI_RATE_DEFAULT_LEVEL 2

/* fluid time scale: left arrow slows it down, right arrow speeds it up */
static const float ui_speed_levels[] = {
    0.15f, 0.30f, 0.55f, 1.0f, 1.6f, 2.5f, 4.0f
};
#define UI_SPEED_LEVELS     ((int)(sizeof(ui_speed_levels) / sizeof(ui_speed_levels[0])))
#define UI_SPEED_DEFAULT_LEVEL 3

void ui_init(UiState *ui)
{
    ui->quit = false;
    ui->paused = false;
    ui->overlay = false;
    ui->audio_on = true;
    ui->want_reset = false;
    ui->want_audio_toggle = false;
    ui->need_full_redraw = false;
    ui->gain = 1.0f;
    ui->rate_level = UI_RATE_DEFAULT_LEVEL;
    ui->token_rate = ui_rate_levels[UI_RATE_DEFAULT_LEVEL];
    ui->vis_style = 0;
    ui->accent = 1;
    ui->speed_level = UI_SPEED_DEFAULT_LEVEL;
    ui->speed = ui_speed_levels[UI_SPEED_DEFAULT_LEVEL];
    ui->overlay_deadline = 0.0;
}

bool ui_key(UiState *ui, int ch, double now)
{
    switch (ch) {
    case 'q':
    case 'Q':
    case 3: /* Ctrl+C arrives as a keystroke in raw mode */
        ui->quit = true;
        return true;
    case ' ':
        ui->paused = !ui->paused;
        return true;
    case 'r':
    case 'R':
        ui->want_reset = true;
        return true;
    case 'm':
    case 'M':
        ui->want_audio_toggle = true;
        return true;
    case '+':
    case '=':
        ui->gain *= UI_GAIN_STEP;
        if (ui->gain > UI_GAIN_MAX)
            ui->gain = UI_GAIN_MAX;
        return true;
    case '-':
    case '_':
        ui->gain /= UI_GAIN_STEP;
        if (ui->gain < UI_GAIN_MIN)
            ui->gain = UI_GAIN_MIN;
        return true;
    case KEY_UP:
        if (ui->rate_level < UI_RATE_LEVELS - 1)
            ui->rate_level++;
        ui->token_rate = ui_rate_levels[ui->rate_level];
        return true;
    case KEY_DOWN:
        if (ui->rate_level > 0)
            ui->rate_level--;
        ui->token_rate = ui_rate_levels[ui->rate_level];
        return true;
    case 'v':
    case 'V':
        ui->vis_style = (ui->vis_style + 1) % FLUID_STYLE_COUNT;
        return true;
    case 27: /* ESC */
        if (ui->overlay) {
            ui->overlay = false;
            ui->overlay_deadline = 0.0;
            ui->need_full_redraw = true;
        }
        return true;
    case KEY_RIGHT:
        if (ui->speed_level < UI_SPEED_LEVELS - 1)
            ui->speed_level++;
        ui->speed = ui_speed_levels[ui->speed_level];
        return true;
    case KEY_LEFT:
        if (ui->speed_level > 0)
            ui->speed_level--;
        ui->speed = ui_speed_levels[ui->speed_level];
        return true;
    case '1': case '2': case '3': case '4': case '5':
    case '6': case '7': case '8': case '9':
        ui->accent = ch - '0';
        return true;
    case 'h':
    case 'H':
    case '?':
    case KEY_F(1):
        ui->overlay = !ui->overlay;
        ui->overlay_deadline = ui->overlay ? now + UI_OVERLAY_SECONDS : 0.0;
        ui->need_full_redraw = true;
        return true;
    default:
        return false;
    }
}

bool ui_tick(UiState *ui, double now)
{
    if (ui->overlay && ui->overlay_deadline > 0.0 && now > ui->overlay_deadline) {
        ui->overlay = false;
        ui->overlay_deadline = 0.0;
        ui->need_full_redraw = true;
        return true;
    }
    return false;
}

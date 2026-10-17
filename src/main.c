/*
 * Hextrix — stationary data, living color.
 *
 * A terminal filled with a fixed grid of hexadecimal tokens whose colors
 * are driven by a slowly flowing liquid field and energized by system
 * audio. Token pairs stay in place but occasionally flicker to new
 * random values at a speed the user controls.
 */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ncurses.h>

#include "audio.h"
#include "fluid.h"
#include "grid.h"
#include "input.h"
#include "renderer.h"
#include "sweep.h"

#define HX_VERSION "1.1.0"

static volatile sig_atomic_t g_signal_quit = 0;
static FILE *g_log = NULL;

static void on_signal(int sig)
{
    (void)sig;
    g_signal_quit = 1;
}

static void log_msg(const char *fmt, ...)
{
    if (g_log == NULL)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "Hextrix %s — terminal fluid color toy\n"
            "\n"
            "Usage: %s [options]\n"
            "       %s sweep [options] [seconds] [color]\n"
            "                             fill the grid, wipe top -> bottom\n"
            "                             with acceleration, then exit\n"
            "                             (see: %s sweep --help)\n"
            "\n"
            "  -a, --alnum      alphanumeric tokens instead of hexadecimal\n"
            "  -n, --no-audio   do not capture system audio\n"
            "  -f, --fps N      target frame rate, 10..120 (default 60)\n"
            "  -h, --help       show this help\n"
            "  -V, --version    show version\n"
            "\n"
            "Keys: q quit      space pause/resume   r reset fluid\n"
            "      m audio     +/- intensity        h/? help\n"
            "      up/down     hex token change speed (down to freeze)\n"
            "      v           visualizer: off (liquid) / wave / rain\n"
            "      1-9         accent color (1 = red, the default)\n"
            "      left/right  fluid speed   esc closes help\n",
            HX_VERSION, prog, prog, prog);
}

/* Help text for the sweep subcommand only. */
static void usage_sweep(FILE *out, const char *prog)
{
    fprintf(out,
            "Hextrix %s — sweep mode\n"
            "\n"
            "Usage: %s sweep [options] [seconds] [color]\n"
            "\n"
            "  -s, --seconds SECS   duration, 0.1..60 (default 1.0)\n"
            "  -c, --color COLOR    accent color (default red)\n"
            "                       1-9, or a name:\n"
            "                       red green yellow blue magenta cyan white\n"
            "                       \"bright red\" \"bright magenta\"\n"
            "  -h, --help           show this help\n"
            "\n"
            "Fill the grid instantly, then wipe it top -> bottom with\n"
            "acceleration and exit when done.\n"
            "\n"
            "Examples:\n"
            "  %s sweep 0.5          0.5s wipe, red accent\n"
            "  %s sweep -s 1 -c 4    1s wipe, blue accent\n"
            "  %s sweep -c cyan      default length, cyan accent\n"
            "  %s sweep 2 magenta    positional form of -s 2 -c magenta\n",
            HX_VERSION, prog, prog, prog, prog, prog);
}

static int accent_from_arg(const char *s);

/* Parse the sweep subcommand's arguments, from argv[start] to argv[argc-1].
 * Accepts flags (-s/--seconds, -c/--color, -h/--help) and positional values
 * (seconds, then color), so "sweep 1 4" == "sweep -s 1 -c 4". Flags may
 * appear before or after the positional values.
 *
 * On success fills *secs (0.1..60) and *accent (1..9) and returns -1.
 * Returns 0 if help was requested (already printed) and 2 on a usage error
 * (message already printed). Defaults: 1.0s, red (accent 1). */
static int parse_sweep(int argc, char **argv, int start,
                       double *secs, int *accent)
{
    double s = 1.0;
    int col = 1;                    /* red, the interactive default */
    bool have_s = false, have_col = false;
    int positional = 0;

    for (int i = start; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage_sweep(stdout, argv[0]);
            return 0;
        } else if (strcmp(a, "-s") == 0 || strcmp(a, "--seconds") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "hextrix: %s needs a value\n", a);
                return 2;
            }
            char *end = NULL;
            double v = strtod(argv[++i], &end);
            if (end == argv[i] || *end != '\0' || !isfinite(v) ||
                v < 0.1 || v > 60.0) {
                fprintf(stderr, "hextrix: sweep duration must be a number "
                                "between 0.1 and 60 seconds\n");
                return 2;
            }
            s = v;
            have_s = true;
        } else if (strcmp(a, "-c") == 0 || strcmp(a, "--color") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "hextrix: %s needs a value\n", a);
                return 2;
            }
            int v = accent_from_arg(argv[++i]);
            if (v == 0) {
                fprintf(stderr, "hextrix: unknown color '%s' (use 1-9 or a "
                                "name like red, green, \"bright magenta\")\n",
                        argv[i]);
                return 2;
            }
            col = v;
            have_col = true;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "hextrix: unknown sweep option '%s'\n", a);
            usage_sweep(stderr, argv[0]);
            return 2;
        } else {
            /* positional: first is seconds, second is color */
            if (positional == 0) {
                if (have_s) {
                    fprintf(stderr, "hextrix: duration given twice "
                                    "(-s and positional)\n");
                    return 2;
                }
                char *end = NULL;
                double v = strtod(a, &end);
                if (end == a || *end != '\0' || !isfinite(v) ||
                    v < 0.1 || v > 60.0) {
                    fprintf(stderr, "hextrix: sweep duration must be a number "
                                    "between 0.1 and 60 seconds\n");
                    return 2;
                }
                s = v;
                have_s = true;
                positional++;
            } else if (positional == 1) {
                if (have_col) {
                    fprintf(stderr, "hextrix: color given twice "
                                    "(-c and positional)\n");
                    return 2;
                }
                int v = accent_from_arg(a);
                if (v == 0) {
                    fprintf(stderr, "hextrix: unknown color '%s' (use 1-9 or "
                                    "a name like red, green, bright magenta)\n",
                            a);
                    return 2;
                }
                col = v;
                have_col = true;
                positional++;
            } else {
                fprintf(stderr, "hextrix: unexpected argument '%s'\n", a);
                usage_sweep(stderr, argv[0]);
                return 2;
            }
        }
    }

    *secs = s;
    *accent = col;
    return -1;
}

static int count_red(const Fluid *f)
{
    int n = 0;
    size_t count = (size_t)f->cols * (size_t)f->rows;
    for (size_t i = 0; i < count; i++)
        n += f->state[i] != 0;
    return n;
}

static const int accent_ansi[9] = {
    1, 2, 3, 4, 5, 6, 7,      /* red green yellow blue magenta cyan white */
    9, 13                     /* bright red, bright magenta */
};

static const char *accent_name(int accent)
{
    static const char *const names[9] = {
        "red", "green", "yellow", "blue", "magenta",
        "cyan", "white", "bright red", "bright magenta"
    };
    if (accent < 1 || accent > 9)
        return "red";
    return names[accent - 1];
}

/* Parse a color argument for the sweep subcommand: "1".."9" or a name.
 * Names are matched case-insensitively, ignoring spaces, '-' and '_',
 * so "bright red", "bright-red" and "BrightRed" are all accent 8.
 * Returns 1..9, or 0 if the argument is not a valid accent. */
static int accent_from_arg(const char *s)
{
    static const char *const names[9] = {
        "red", "green", "yellow", "blue", "magenta",
        "cyan", "white", "brightred", "brightmagenta"
    };
    if (s == NULL || *s == '\0')
        return 0;
    if (s[0] >= '1' && s[0] <= '9' && s[1] == '\0')
        return s[0] - '0';

    char norm[32];
    size_t j = 0;
    for (size_t i = 0; s[i] != '\0' && j < sizeof norm - 1; i++) {
        char c = s[i];
        if (c == ' ' || c == '-' || c == '_')
            continue;
        norm[j++] = (char)tolower((unsigned char)c);
    }
    norm[j] = '\0';
    for (int i = 0; i < 9; i++) {
        if (strcmp(norm, names[i]) == 0)
            return i + 1;
    }
    return 0;
}

static const char *audio_status_name(int st)
{
    switch (st) {
    case 0: return "unavailable";
    case 1: return "connecting";
    case 2: return "active";
    default: return "error";
    }
}

static bool do_resize(Grid *grid, Fluid *fluid, Renderer *ren,
                      int term_cols, int term_rows)
{
    if (!grid_resize(grid, term_cols, term_rows)) {
        log_msg("resize: grid allocation failed (%dx%d)", term_cols, term_rows);
        return false;
    }
    if (!fluid_resize(fluid, grid->cols, grid->rows)) {
        log_msg("resize: fluid allocation failed (%dx%d)", grid->cols, grid->rows);
        return false;
    }
    renderer_resize(ren, grid->cols, grid->rows);
    renderer_invalidate(ren);
    clear();
    log_msg("resize: terminal %dx%d -> grid %dx%d",
            term_cols, term_rows, grid->cols, grid->rows);
    return true;
}

int main(int argc, char **argv)
{
    GridMode mode = GRID_HEX;
    bool want_audio = true;
    double fps = 60.0;
    double sweep_secs = -1.0;       /* > 0: one-shot sweep subcommand */
    int sweep_accent = 1;           /* sweep accent color, 1 = red */
    const char *prog = argc > 0 ? argv[0] : "hextrix";

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-a") == 0 || strcmp(a, "--alnum") == 0) {
            mode = GRID_ALNUM;
        } else if (strcmp(a, "-n") == 0 || strcmp(a, "--no-audio") == 0) {
            want_audio = false;
        } else if (strcmp(a, "-f") == 0 || strcmp(a, "--fps") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "hextrix: %s needs a value\n", a);
                return 2;
            }
            fps = atof(argv[++i]);
            if (fps < 10.0)
                fps = 10.0;
            if (fps > 120.0)
                fps = 120.0;
        } else if (strcmp(a, "sweep") == 0) {
            if (sweep_secs > 0.0) {
                fprintf(stderr, "hextrix: sweep specified twice\n");
                return 2;
            }
            sweep_secs = 1.0;       /* placeholder, overwritten below */
            int rc = parse_sweep(argc, argv, i + 1, &sweep_secs, &sweep_accent);
            if (rc == 0)
                return 0;           /* sweep --help was printed */
            if (rc != -1)
                return rc;          /* usage error, message already printed */
            i = argc;               /* parse_sweep consumed the rest */
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout, prog);
            return 0;
        } else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("hextrix %s\n", HX_VERSION);
            return 0;
        } else {
            fprintf(stderr, "hextrix: unknown option '%s'\n", a);
            usage(stderr, prog);
            return 2;
        }
    }

    const char *log_path = getenv("HEXTRIX_LOG");
    if (log_path != NULL && log_path[0] != '\0') {
        g_log = fopen(log_path, "w");
        if (g_log != NULL)
            log_msg("hextrix %s starting, fps=%.1f mode=%s audio=%s",
                    HX_VERSION, fps,
                    mode == GRID_HEX ? "hex" : "alnum",
                    want_audio ? "on" : "off");
        else
            fprintf(stderr, "hextrix: cannot open log %s: %s\n",
                    log_path, strerror(errno));
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    int exit_code = 0;
    Grid grid;
    Fluid fluid;
    Renderer ren;
    bool inited = false;

    memset(&grid, 0, sizeof(grid));
    memset(&fluid, 0, sizeof(fluid));
    memset(&ren, 0, sizeof(ren));

    if (initscr() == NULL) {
        fprintf(stderr, "hextrix: could not initialize the terminal\n");
        if (g_log != NULL)
            fclose(g_log);
        return 1;
    }
    inited = true;

    raw();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    set_escdelay(25);       /* bare Esc must not stall the whole app */
    curs_set(0);

    bool color_ok = false;
    bool dim_pair = false;
    bool chip_pair = false;
    if (has_colors()) {
        start_color();
        if (use_default_colors() == OK) {
            color_ok = true;
            for (int i = 0; i < 9; i++) {
                if (init_pair(i + 1, accent_ansi[i], -1) != OK) {
                    color_ok = false;
                    break;
                }
            }
            /* pair 10: bright black = deep gray body text, extra A_DIM */
            if (color_ok && init_pair(10, 8, -1) == OK)
                dim_pair = true;
            /* pair 11: dim plate behind the centered hint labels */
            if (color_ok)
                chip_pair = init_pair(11, -1, 8) == OK;
        }
    }
    log_msg("colors: %s", color_ok ? "9 accents (1=red) + dim body"
                                   : "monochrome (no color support)");
    log_msg("dim: %s", dim_pair ? "color 8 + A_DIM" : "A_DIM only");

    refresh();

    int term_rows, term_cols;
    getmaxyx(stdscr, term_rows, term_cols);

    grid_init(&grid, term_cols, term_rows, mode);
    log_msg("grid: terminal %dx%d -> %d x %d tokens (cell width %d)",
            term_cols, term_rows, grid.cols, grid.rows, GRID_CELL_W);

    UiState ui;
    ui_init(&ui);

    if (sweep_secs > 0.0) {
        log_msg("sweep: start duration=%.2fs color=%s",
                sweep_secs, accent_name(sweep_accent));
        exit_code = sweep_run((float)sweep_secs, &grid, sweep_accent,
                              color_ok, dim_pair, &g_signal_quit);
        log_msg("sweep: finished exit=%d", exit_code);
        goto cleanup;
    }

    fluid_init(&fluid, grid.cols, grid.rows);
    renderer_init(&ren, grid.cols, grid.rows, color_ok, dim_pair);
    ren.chip_pair = chip_pair;
    ren.accent = ui.accent;

    bool audio_ok = false;
    if (want_audio) {
        audio_ok = audio_init();
        log_msg("audio: init %s, backend %s", audio_ok ? "ok" : "failed",
                audio_backend());
    } else {
        log_msg("audio: disabled by option");
    }
    ui.audio_on = audio_ok;
    if (audio_ok)
        audio_set_enabled(true);

    const double frame_dt = 1.0 / fps;
    double last = now_sec();
    double next_frame = last;
    double fps_start = last;
    int fps_frames = 0;
    float tok_accum = 0.0f;
    int last_style = ui.vis_style;
    int last_accent = ui.accent;
    int last_speed = ui.speed_level;
    int last_cols = term_cols;
    int last_rows = term_rows;

    while (!ui.quit && !g_signal_quit) {
        double t = now_sec();
        double wait = next_frame - t;
        int wait_ms = wait > 0.0 ? (int)(wait * 1000.0 + 0.5) : 0;
        if (wait_ms > (int)(frame_dt * 1000.0) + 2)
            wait_ms = (int)(frame_dt * 1000.0) + 2;

        wtimeout(stdscr, wait_ms);
        int ch = getch();
        if (ch != ERR) {
            wtimeout(stdscr, 0);
            while (ch != ERR && !ui.quit && !g_signal_quit) {
                if (ch != KEY_RESIZE)
                    ui_key(&ui, ch, now_sec());
                ch = getch();
            }
        }
        if (ui.quit || g_signal_quit)
            break;

        /* terminal size may change without a KEY_RESIZE on some terminals */
        getmaxyx(stdscr, term_rows, term_cols);
        if (term_cols != last_cols || term_rows != last_rows) {
            do_resize(&grid, &fluid, &ren, term_cols, term_rows);
            last_cols = term_cols;
            last_rows = term_rows;
        }

        double frame_now = now_sec();
        ui_tick(&ui, frame_now);
        if (ui.need_full_redraw) {
            renderer_invalidate(&ren);
            ui.need_full_redraw = false;
        }

        double dt = frame_now - last;
        if (dt < 0.0)
            dt = 0.0;
        if (dt > 0.1)
            dt = 0.1;
        last = frame_now;

        /* audio -> fluid */
        float a_bass = 0.0f, a_mid = 0.0f, a_treble = 0.0f, a_rms = 0.0f;
        if (ui.audio_on && audio_ok && audio_status() == AUDIO_ACTIVE)
            audio_read(&a_bass, &a_mid, &a_treble, &a_rms);
        fluid_audio(&fluid, a_bass, a_mid, a_treble, a_rms, dt);

        if (ui.want_reset) {
            fluid_reset(&fluid);
            ui.want_reset = false;
            log_msg("reset: fluid cleared, grid untouched");
        }
        if (ui.want_audio_toggle) {
            ui.audio_on = !ui.audio_on;
            if (audio_ok)
                audio_set_enabled(ui.audio_on);
            ui.want_audio_toggle = false;
            log_msg("audio: %s", ui.audio_on ? "on" : "off");
        }

        fluid.gain = ui.gain;
        fluid.style = ui.vis_style;
        ren.accent = ui.accent;
        if (ui.vis_style != last_style) {
            last_style = ui.vis_style;
            log_msg("style: %s", fluid_style_name(ui.vis_style));
            renderer_flash(&ren, fluid_style_name(ui.vis_style), frame_now);
        }
        if (ui.accent != last_accent) {
            last_accent = ui.accent;
            renderer_invalidate(&ren);        /* repaint in the new color */
            log_msg("accent: %s", accent_name(ui.accent));
            renderer_flash(&ren, accent_name(ui.accent), frame_now);
        }
        if (ui.speed_level != last_speed) {
            last_speed = ui.speed_level;
            char sbuf[32];
            snprintf(sbuf, sizeof sbuf, "speed %.2gx", ui.speed);
            log_msg("speed: %.2f", (double)ui.speed);
            renderer_flash(&ren, sbuf, frame_now);
        }
        if (!ui.paused) {
            fluid_step(&fluid, dt * ui.speed);

            /* token values flicker to new random pairs at the set rate */
            if (ui.token_rate > 0.0f) {
                size_t cells = (size_t)grid.cols * (size_t)grid.rows;
                tok_accum += (float)((double)ui.token_rate * (double)cells * dt);
                int k = (int)tok_accum;
                tok_accum -= (float)k;
                if (k > 0)
                    grid_mutate(&grid, k);
            }
        }

        renderer_draw(&ren, &grid, &fluid);
        renderer_overlay(&ren, ui.overlay, frame_now);
        doupdate();

        fps_frames++;
        if (frame_now - fps_start >= 2.0) {
            double measured = (double)fps_frames / (frame_now - fps_start);
            log_msg("fps=%.1f red=%d/%d paused=%d gain=%.2f rate=%.4f "
                    "style=%s audio=%s bass=%.3f mid=%.3f treble=%.3f rms=%.3f",
                    measured,
                    count_red(&fluid), fluid.cols * fluid.rows,
                    ui.paused ? 1 : 0, ui.gain, (double)ui.token_rate,
                    fluid_style_name(fluid.style),
                    audio_status_name((int)audio_status()),
                    (double)fluid.a_bass, (double)fluid.a_mid,
                    (double)fluid.a_treble, (double)fluid.a_rms);
            fps_frames = 0;
            fps_start = frame_now;
        }

        next_frame += frame_dt;
        if (now_sec() > next_frame + 0.5)
            next_frame = now_sec();
    }

cleanup:
    log_msg("hextrix exiting (quit=%d signal=%d)", ui.quit ? 1 : 0,
            (int)g_signal_quit);

    if (inited) {
        refresh();
        curs_set(1);
        endwin();
    }
    audio_shutdown();
    renderer_free(&ren);
    fluid_free(&fluid);
    grid_free(&grid);
    if (g_log != NULL)
        fclose(g_log);

    return exit_code;
}

/* macos/eng_ui_simple.h -- the Mac build's Esc menu: ONE page of the settings a player needs, instead of the arcade
 * engine's tabs (File / Display / Audio / Controls / ...), which stay one row away under "Advanced settings".
 *
 *   Resume
 *   Save game      a list of 8 slots (when / where): Enter saves the moment there (engine/ss22_state.c)
 *   Load game      the same list: Enter goes back to that moment
 *   Display        Window (fills the desktop, menu bar and Dock visible) / Borderless (the whole screen, no title bar,
 *                  no Space switch) / Fullscreen (the display's own mode)
 *   Widescreen     On / Off
 *   Crosshair      Off / Classic / the bundled images (engine/eng_crosshair.c)
 *   HD textures    On / Off: the AI-upscaled textures (macos/tex_hd.h; only a game that has them)
 *   Mouse speed    Normal (the crosshair under the pointer) / 125..300 %: how far the crosshair goes for a move (engine/ss22_input.c)
 *   Smooth motion  On / Off: an in-between picture between two of the board's, at 120 Hz (engine/slave_list.c, tc2/src/tc2_render.c)
 *   Volume         0..100 %
 *   Difficulty     the game's own operator setting (eng_ui_set_game_options)
 *   Lives          the game's own operator setting
 *   Continues      On / Off: Off = no continue at the last life, game over, start again (the game's own hook)
 *   Restart game   difficulty and lives take effect when a game starts from power-on
 *   Advanced settings
 *   Quit
 *
 * No coins anywhere: the games run in their own free play (timecris/src/tc_game.c, tc2/src/tc2_mem.c).
 * Included once by each copy of eng_ui.c (engine/ and tc2/src/eng/), after its standard pages are defined. */
#ifndef ENG_UI_SIMPLE_H
#define ENG_UI_SIMPLE_H

static const eng_ui_game_opts *game_opts;
void eng_ui_set_game_options(const eng_ui_game_opts *o) { game_opts = o; }
static bool opts_changed;                                     /* a game option changed since the menu opened: say "restart" */

enum { S_RESUME, S_SAVE, S_LOAD, S_DISPLAY, S_WIDE, S_CROSS, S_MOUSE, S_HD, S_SMOOTH, S_VOLUME, S_DIFF, S_LIVES, S_CONT, S_RESTART, S_ADV, S_QUIT, S_N };
/* HD textures (macos/tex_hd.h): weak fallbacks here, so a game without the texture bake (Time Crisis II) links and shows no row;
 * Time Crisis 1's real ones (engine/tex_bake.c) take their place at link time */
__attribute__((weak)) int  tex_hd_available(void) { return 0; }
__attribute__((weak)) int  tex_hd_is_on(void) { return 0; }
__attribute__((weak)) void tex_hd_set(int on) { (void)on; }
static bool hd_row(void) { return tex_hd_available(); }
/* motion smoothing: each game defines these (engine/ss22_gl.c, tc2/src/tc2_window.c); none = no row */
__attribute__((weak)) int  eng_smooth_available(void) { return 0; }
__attribute__((weak)) int  eng_smooth_is_on(void) { return 0; }
__attribute__((weak)) void eng_smooth_set(int on) { (void)on; }
/* the mouse's reach (both copies of ss22_input.c define these) */
__attribute__((weak)) int  ss22_input_mouse_speed(void) { return 0; }
__attribute__((weak)) void ss22_input_set_mouse_speed(int pct) { (void)pct; }
static const int mouse_steps[5] = { 100, 125, 150, 200, 300 };
/* save states: each game defines these (engine/ss22_state.c); none = no Save / Load rows */
__attribute__((weak)) int  eng_state_slots(void) { return 0; }
__attribute__((weak)) void eng_state_request(int save, int slot) { (void)save; (void)slot; }
__attribute__((weak)) int  eng_state_slot_info(int slot, char *buf, size_t n) { (void)slot; snprintf(buf, n, "empty"); return 0; }
#define SLOTS_MAX 16
static int  simple_list;                                       /* 0 the settings; 1 the Save list, 2 the Load list */
static char slot_text[SLOTS_MAX][96]; static bool slot_full[SLOTS_MAX];
static int  slot_n(void) { const int n = eng_state_slots(); return n > SLOTS_MAX ? SLOTS_MAX : n; }
static void slots_refresh(void) { for (int i = 0; i < slot_n(); i++) slot_full[i] = eng_state_slot_info(i, slot_text[i], sizeof slot_text[i]) != 0; }
static bool simple_back(void) { if (!simple_list) return false; row = simple_list == 1 ? 1 : 2; simple_list = 0; return true; }
static int mouse_step(void) { int b = 0; for (int i = 0; i < 5; i++) if (ss22_input_mouse_speed() >= mouse_steps[i]) b = i; return b; }
static bool row_shown(int s)                                  /* the game's options and HD textures only when the game has them */
{
    if (s == S_HD) return hd_row();
    if (s == S_SMOOTH) return eng_smooth_available();
    if (s == S_MOUSE) return ss22_input_mouse_speed() > 0;
    if (s == S_SAVE || s == S_LOAD) return slot_n() > 0;
    if (s == S_DIFF || s == S_LIVES) return game_opts != NULL;
    if (s == S_CONT) return game_opts && game_opts->count(2) > 0;
    return true;
}
static int simple_row(int r)                                  /* rows shown -> S_* */
{
    for (int s = 0; s < S_N; s++) if (row_shown(s) && r-- == 0) return s;
    return S_QUIT;
}
static int simple_n(void) { if (simple_list) return slot_n() + 1; int n = 0; for (int s = 0; s < S_N; s++) n += row_shown(s); return n; }
static bool simple_val(int r)
{
    if (simple_list) return false;
    const int s = simple_row(r);
    return s == S_DISPLAY || s == S_WIDE || s == S_CROSS || s == S_MOUSE || s == S_HD || s == S_SMOOTH || s == S_VOLUME || s == S_DIFF || s == S_LIVES || s == S_CONT;
}
static int display_mode(void) { return g_eng_disp.winmode == 0 ? 0 : g_eng_disp.winmode == 1 ? 1 : 2; }
static const char *const display_names[3] = { "Window", "Borderless", "Fullscreen" };
static void simple_text(int r, char *l, size_t ln, char *v, size_t vn)
{
    *v = 0;
    if (simple_list) {                                        /* a slot list: "Slot 3   04 Nov 21:30  -  playing, 2 lives left" */
        if (r < slot_n()) snprintf(l, ln, "Slot %d     %s", r + 1, slot_text[r]); else snprintf(l, ln, "Back");
        return;
    }
    switch (simple_row(r)) {
    case S_RESUME:  snprintf(l, ln, "Resume"); break;
    case S_SAVE:    snprintf(l, ln, "Save game"); break;
    case S_LOAD:    snprintf(l, ln, "Load game"); break;
    case S_DISPLAY: snprintf(l, ln, "Display"); snprintf(v, vn, "%s", display_names[display_mode()]); break;
    case S_WIDE:    snprintf(l, ln, "Widescreen"); snprintf(v, vn, "%s", g_eng_disp.wide ? "On" : "Off"); break;
    case S_CROSS: {
        snprintf(l, ln, "Crosshair");
        if (!g_eng_disp.crosshair) { snprintf(v, vn, "Off"); break; }
        const char *n = eng_xhair_name();
        if (!strcmp(n, "built-in")) { snprintf(v, vn, "Classic"); break; }
        char b[64]; snprintf(b, sizeof b, "%s", n);          /* "1 ring green.png" -> "Ring green" */
        char *dot = strrchr(b, '.'); if (dot) *dot = 0;
        char *p = b; while (*p >= '0' && *p <= '9') p++; while (*p == ' ') p++;
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
        snprintf(v, vn, "%s", p);
        break;
    }
    case S_HD:      snprintf(l, ln, "HD textures"); snprintf(v, vn, "%s", tex_hd_is_on() ? "On" : "Off"); break;
    case S_MOUSE:   snprintf(l, ln, "Mouse speed"); if (mouse_step() == 0) snprintf(v, vn, "Normal"); else snprintf(v, vn, "%d%%", mouse_steps[mouse_step()]); break;
    case S_SMOOTH:  snprintf(l, ln, "Smooth motion"); snprintf(v, vn, "%s", eng_smooth_is_on() ? "On" : "Off"); break;
    case S_VOLUME:  snprintf(l, ln, "Volume"); snprintf(v, vn, "%d%%", g_eng_disp.volume); break;
    case S_DIFF:    snprintf(l, ln, "Difficulty"); snprintf(v, vn, "%s", game_opts->name(0, game_opts->get(0))); break;
    case S_LIVES:   snprintf(l, ln, "Lives"); snprintf(v, vn, "%s", game_opts->name(1, game_opts->get(1))); break;
    case S_CONT:    snprintf(l, ln, "Continues"); snprintf(v, vn, "%s", game_opts->name(2, game_opts->get(2))); break;
    case S_RESTART: snprintf(l, ln, "Restart game"); break;
    case S_ADV:     snprintf(l, ln, "Advanced settings"); break;
    case S_QUIT:    snprintf(l, ln, "Quit"); break;
    }
}
static void simple_show_all(void);
static void simple_change(int r, int dir)
{
    if (simple_list) {                                        /* a slot list: Enter saves there / loads it; Back returns */
        if (dir != 0) return;
        if (r >= slot_n()) { simple_back(); return; }
        if (simple_list == 2 && !slot_full[r]) return;        /* nothing to load */
        eng_state_request(simple_list == 1, r);
        simple_list = 0; open_ = false;                       /* back to the game: the save / load happens within a frame */
        return;
    }
    const int s = simple_row(r);
    const int d = dir == 0 ? 1 : dir;                         /* Enter / click steps forward */
    switch (s) {
    case S_RESUME:  if (dir == 0) open_ = false; break;
    case S_SAVE:    if (dir == 0) { simple_list = 1; slots_refresh(); row = 0; } break;
    case S_LOAD:    if (dir == 0) { simple_list = 2; slots_refresh(); row = 0; } break;
    case S_DISPLAY: {
        const int m = cyc(display_mode(), d, 3);
        if (m == 0 && !g_eng_disp_fill) { g_eng_disp_fill = 1; eng_cfg_set_int("window_fill", 1); }
        eng_disp_set_winmode(m);
        break;
    }
    case S_WIDE:    eng_disp_set_wide(!g_eng_disp.wide); break;
    case S_CROSS: {                                           /* choices: 0 Off, 1 Classic (built-in), 2.. each image */
        const int imgs = eng_xhair_count(), n = imgs + 2;
        int k = 0;                                            /* the image now: steps forward to reach built-in again */
        while (strcmp(eng_xhair_name(), "built-in") && k <= imgs) { eng_xhair_step(1); k++; }
        const int cur = !g_eng_disp.crosshair ? 0 : 1 + (k ? imgs + 1 - k : 0);
        const int want = cyc(cur, d, n);                      /* (now at built-in: step forward to the wanted image) */
        eng_disp_set_crosshair(want != 0);
        for (int i = 1; i < want; i++) eng_xhair_step(1);
        if (want == 0) for (int i = 0; i < (k ? imgs + 1 - k : 0); i++) eng_xhair_step(1);   /* Off keeps the image it had */
        break;
    }
    case S_HD:      { const int on = !tex_hd_is_on(); tex_hd_set(on); eng_cfg_set_int("hd_textures", on); break; }
    case S_MOUSE:   ss22_input_set_mouse_speed(mouse_steps[cyc(mouse_step(), d, 5)]); break;
    case S_SMOOTH:  { const int on = !eng_smooth_is_on(); eng_smooth_set(on); eng_cfg_set_int("motion_smooth", on); break; }
    case S_VOLUME: {
        int v = g_eng_disp.volume + 10 * d;
        if (dir == 0 && g_eng_disp.volume >= 100) v = 0;
        eng_disp_set_volume(v < 0 ? 0 : v > 100 ? 100 : v);
        break;
    }
    case S_DIFF: case S_LIVES: case S_CONT: {
        const int w = s == S_DIFF ? 0 : s == S_LIVES ? 1 : 2;
        game_opts->set(w, cyc(game_opts->get(w), d, game_opts->count(w)));
        if (w < 2) opts_changed = true;                       /* Continues applies at once */
        break;
    }
    case S_RESTART: if (dir == 0) { restart_req = true; quit_req = true; } break;
    case S_ADV:     if (dir == 0) simple_show_all(); break;
    case S_QUIT:    if (dir == 0) quit_req = true; break;
    }
}
static void simple_notes(void (*line)(const char *fmt, ...))
{
    if (opts_changed) line("Difficulty and lives apply after Restart game.");
    line("Mouse: aim. Left click: fire. Right click or Z: start a game; step out of cover (let go to reload).");
    line("Esc: back to the game.");
}
static const eng_ui_page page_simple = { "Settings", 460, 150, 30, simple_n, simple_val, NULL, simple_text, simple_change, simple_notes };

/* the full menu ("Advanced settings") and back: the engine's tabs are kept aside while the simple page is shown */
static const eng_ui_page *pages_all[MAXPAGES]; static int npages_all;
static void simple_keep_all(void)                             /* after eng_ui_init built the standard list */
{
    memcpy(pages_all, pages, sizeof pages_all); npages_all = npages;
    pages[0] = &page_simple; npages = 1; tab = 0; row = 0;
}
static void simple_show_all(void)
{
    npages = 0; pages[npages++] = &page_simple;
    for (int i = 0; i < npages_all && npages < MAXPAGES; i++) pages[npages++] = pages_all[i];
    tab = 2; row = 0;                                         /* Display */
}
static void simple_on_open(void)                              /* every time the menu opens: the simple page */
{
    if (!npages_all) return;
    pages[0] = &page_simple; npages = 1; tab = 0; row = 0; opts_changed = false; simple_list = 0;
}

/* ---- the pause screen: the simple page drawn large and centred over the dimmed game (the tabs keep the engine's bar) ---- */
static struct nk_font *simple_font, *simple_title_font;
static void simple_add_fonts(struct nk_font_atlas *atlas, float scale)          /* between nk_sdl_font_stash_begin and _end */
{
    struct nk_font_config c = nk_font_config(0);
    simple_font = nk_font_atlas_add_default(atlas, 24 * scale, &c);
    simple_title_font = nk_font_atlas_add_default(atlas, 36 * scale, &c);
}
static void simple_fonts_done(float scale) { simple_font->handle.height /= scale; simple_title_font->handle.height /= scale; }
static bool simple_activate(int r) { simple_change(r, 0); return true; }
static bool simple_draw(int ww, int wh)                        /* true: drawn (the caller renders); false: the tabs are showing */
{
    if (npages != 1 || pages[0] != &page_simple || !simple_font) return false;
    const struct nk_user_font *small = ctx->style.font;
    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background, nk_style_item_color(nk_rgba(0, 0, 0, 150)));
    if (nk_begin(ctx, "simple_dim", nk_rect(0, 0, (float)ww, (float)wh), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_NO_INPUT | NK_WINDOW_BACKGROUND)) {}
    nk_end(ctx);
    nk_style_pop_style_item(ctx);

    const int n = simple_n();
    const float rh = 44, pw = ww - 40 < 640 ? (float)ww - 40 : 640;
    float ph = 64 + n * (rh + 4) + 64;
    if (ph > wh - 40) ph = (float)wh - 40;
    const struct nk_rect r = nk_rect(((float)ww - pw) / 2, ((float)wh - ph) / 2, pw, ph);
    nk_style_push_style_item(ctx, &ctx->style.window.fixed_background, nk_style_item_color(nk_rgba(18, 20, 28, 235)));
    nk_style_set_font(ctx, &simple_font->handle);
    if (nk_begin(ctx, "simple_panel", r, NK_WINDOW_BORDER)) {
        nk_window_set_bounds(ctx, "simple_panel", r);
        char title[96];
        if (simple_list) snprintf(title, sizeof title, "%s", simple_list == 1 ? "Save game" : "Load game");
        else snprintf(title, sizeof title, "%s", SDL_GetWindowTitle(uwin));
        nk_style_set_font(ctx, &simple_title_font->handle);
        nk_layout_row_dynamic(ctx, 52, 1);
        nk_label(ctx, title, NK_TEXT_CENTERED);
        nk_style_set_font(ctx, &simple_font->handle);
        for (int i = 0; i < n; i++) {
            char label[64], value[96];
            simple_text(i, label, sizeof label, value, sizeof value);
            const bool was = i == row;
            if (simple_val(i)) {
                nk_layout_row_template_begin(ctx, rh);
                nk_layout_row_template_push_dynamic(ctx);
                nk_layout_row_template_push_static(ctx, 40);
                nk_layout_row_template_push_static(ctx, 230);
                nk_layout_row_template_push_static(ctx, 40);
                nk_layout_row_template_end(ctx);
                if (nk_select_label(ctx, label, NK_TEXT_LEFT, was) != was) row = i;
                if (nk_button_symbol(ctx, NK_SYMBOL_TRIANGLE_LEFT)) { row = i; simple_change(i, -1); }
                if (nk_button_label(ctx, value)) { row = i; simple_change(i, 0); }
                if (nk_button_symbol(ctx, NK_SYMBOL_TRIANGLE_RIGHT)) { row = i; simple_change(i, +1); }
            } else {
                nk_layout_row_dynamic(ctx, rh, 1);
                if (nk_select_label(ctx, label, NK_TEXT_CENTERED, was) != was) { row = i; simple_activate(i); }
            }
        }
        nk_style_set_font(ctx, small);
        nk_layout_row_dynamic(ctx, 20, 1);
        if (simple_list == 1) nk_label(ctx, "Enter or click a slot: the game is saved there (an old save in it is replaced).  Esc: back.", NK_TEXT_CENTERED);
        else if (simple_list == 2) nk_label(ctx, "Enter or click a slot: the game goes back to that moment.  Esc: back.", NK_TEXT_CENTERED);
        else {
            if (opts_changed) nk_label(ctx, "Difficulty and lives apply after Restart game.", NK_TEXT_CENTERED);
            nk_label(ctx, "Arrows / Enter or the mouse.  Esc: back to the game.", NK_TEXT_CENTERED);
        }
    }
    nk_end(ctx);
    nk_style_set_font(ctx, small);
    nk_style_pop_style_item(ctx);
    return true;
}
#endif

/* macos/launcher.c -- the .app bundle's executable: a compiled Mach-O that
 * execv()s the game in the same process.
 *
 * macOS Game Mode classifies a game from the program LaunchServices spawns; a
 * #! script bundle executable spawns as the shell and is never classified.
 * This launcher does what the old script did -- find the ROM folder (inside a
 * self-contained app: Contents/Resources/extracted), run the game from the
 * folder for the player's settings, saves and high scores, log to
 * ~/Library/Logs/<name>/last-run.log -- and then becomes the game (same pid). The game opens in its saved window mode; on the Mac that is
 * by default a window filling the desktop (engine/eng_display.c, window_fill).
 *
 * Built by macos/package.sh with the per-game values below:
 *   -DLAUNCH_NAME="Time Crisis"   -DLAUNCH_BIN="Time Crisis"
 *   -DLAUNCH_ROM_ENV="TIMECRIS_ROMS"  -DLAUNCH_ROMS="<default extracted dir>"
 *   -DLAUNCH_ROMS_ARG=1 (TC1 takes the dir as argv[1]) */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <copyfile.h>
#include <strings.h>
#include <time.h>
#ifndef LAUNCH_PLAYER_ROMS
#define LAUNCH_PLAYER_ROMS 0      /* 1: a release app with no game data -- it asks for the player's own MAME zip (player_roms) */
#endif
#ifndef LAUNCH_SET
#define LAUNCH_SET "timecris"     /* the MAME set: its zips are <set>*.zip (+ namco*.zip device zips) */
#endif
#ifndef LAUNCH_ZIP_HELP
#define LAUNCH_ZIP_HELP LAUNCH_SET ".zip"
#endif

static void fail(const char *what, const char *path)
{
    fprintf(stderr, "%s: %s %s: %s\n", LAUNCH_NAME, what, path, strerror(errno));
    exit(1);
}

static int has_zip(const char *dir)
{
    DIR *d = opendir(dir); int n = 0;
    for (struct dirent *e; d && (e = readdir(d)); ) { size_t k = strlen(e->d_name); if (e->d_name[0] != '.' && k > 4 && !strcasecmp(e->d_name + k - 4, ".zip")) n++; }
    if (d) closedir(d);
    return n;
}

/* the last run said the ROMs were missing or wrong (engine/ss22_run.c, tc2/src/tc2_rom.c write it to the log) */
static int last_run_rom_failure(const char *home)
{
    char p[PATH_MAX], line[2048]; int bad = 0;
    snprintf(p, sizeof p, "%s/Library/Logs/%s/last-run.log", home, LAUNCH_NAME);
    FILE *f = fopen(p, "r");
    while (f && fgets(line, sizeof line, f))
        if (strstr(line, "needs its ROM") || strstr(line, "[ROM] Put ") || strstr(line, "could not read its ROM")) bad = 1;
    if (f) fclose(f);
    return bad;
}

/* A release app's first start: no game data is included, so the player chooses their own MAME zip once (a macOS file dialog), and it is
 * copied to <work>/roms, where the game finds it and unpacks it into <work>/extracted (engine/romzip.c checks every chip's size and
 * CRC). A zip the game turned down last time is put aside in roms/not-used and the player is asked again. */
static void player_roms(const char *work, const char *home)
{
    char zips[PATH_MAX], ext[PATH_MAX];
    snprintf(zips, sizeof zips, "%s/roms", work); mkdir(zips, 0755);
    snprintf(ext, sizeof ext, "%s/extracted", work);
    const int failed = last_run_rom_failure(home);
    if (has_zip(zips) && !failed) return;                  /* chosen before: the game unpacks it (or already has) */
    if (!has_zip(zips) && !failed) {                       /* unpacked before and the zip since removed: nothing to ask */
        DIR *d = opendir(ext); int n = 0;
        for (struct dirent *e; d && (e = readdir(d)); ) if (e->d_name[0] != '.') n++;
        if (d) closedir(d);
        if (n > 0) return;
    }
    if (failed) {                                          /* the zip the game could not use: aside, not deleted */
        char aside[PATH_MAX]; snprintf(aside, sizeof aside, "%s/not-used", zips); mkdir(aside, 0755);
        DIR *d = opendir(zips);
        for (struct dirent *e; d && (e = readdir(d)); ) {
            size_t k = strlen(e->d_name);
            if (e->d_name[0] == '.' || k <= 4 || strcasecmp(e->d_name + k - 4, ".zip")) continue;
            char a[PATH_MAX], b[PATH_MAX]; snprintf(a, sizeof a, "%s/%s", zips, e->d_name);
            snprintf(b, sizeof b, "%s/%ld-%s", aside, (long)time(NULL), e->d_name);
            rename(a, b);
        }
        if (d) closedir(d);
    }
    char script[PATH_MAX], res[PATH_MAX], self[PATH_MAX], real[PATH_MAX]; uint32_t n = sizeof self;
    snprintf(script, sizeof script, "%s/.choose-roms.applescript", work);
    res[0] = 0;
    if (_NSGetExecutablePath(self, &n) == 0 && realpath(self, real)) snprintf(res, sizeof res, "%s/../Resources/AppIcon.icns", dirname(real));
    FILE *f = fopen(script, "w");
    if (!f) fail("cannot write", script);
    fprintf(f,
        "activate\n"
        "set msg to \"%s%s needs the original arcade game data. It is not included with this app.\" & return & return & "
        "\"Choose your own copy of the MAME ROM set: %s\" & return & return & \"You only need to do this once.\"\n"
        "set r to button returned of (display dialog msg buttons {\"Quit\", \"Choose File...\"} default button 2 with title \"%s\"%s%s%s)\n"
        "if r is \"Quit\" then return \"\"\n"
        "set fs to choose file with prompt \"Choose %s\" of type {\"zip\", \"public.zip-archive\", \"com.pkware.zip-archive\"} with multiple selections allowed\n"
        "set out to \"\"\n"
        "repeat with x in fs\n  set out to out & POSIX path of x & linefeed\nend repeat\n"
        "return out\n",
        failed ? "The file you chose last time is not the right one (see the message the game showed).\" & return & return & \"" : "",
        LAUNCH_NAME, LAUNCH_ZIP_HELP, LAUNCH_NAME,
        res[0] ? " with icon POSIX file \"" : " with icon note", res[0] ? res : "", res[0] ? "\"" : "", LAUNCH_ZIP_HELP);
    fclose(f);
    char cmd[PATH_MAX + 64]; snprintf(cmd, sizeof cmd, "/usr/bin/osascript '%s' 2>/dev/null", script);
    const char *answer = getenv("LAUNCH_TEST_CHOICE");     /* tests: a file the player "chose", without the dialog */
    if (answer && *answer) snprintf(cmd, sizeof cmd, "/bin/echo '%s'", answer);
    FILE *p = popen(cmd, "r");
    char line[PATH_MAX]; int copied = 0, named = 0;
    while (p && fgets(line, sizeof line, p)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s", line);
        const char *base = basename(tmp);
        char dst[PATH_MAX];
        /* the game looks for <set>*.zip and namco*.zip: a set saved under another name ("Time Crisis.zip") is copied as <set>.zip */
        if (!strncasecmp(base, LAUNCH_SET, strlen(LAUNCH_SET)) || !strncasecmp(base, "namco", 5)) snprintf(dst, sizeof dst, "%s/%s", zips, base);
        else if (!named++) snprintf(dst, sizeof dst, "%s/%s.zip", zips, LAUNCH_SET);
        else snprintf(dst, sizeof dst, "%s/%s", zips, base);
        if (copyfile(line, dst, NULL, COPYFILE_DATA | COPYFILE_CLONE) == 0) copied++;
        else fprintf(stderr, "%s: cannot copy %s: %s\n", LAUNCH_NAME, line, strerror(errno));
    }
    if (p) pclose(p);
    unlink(script);
    if (!copied) exit(0);                                  /* Quit, or the dialog closed: nothing to start */
}

int main(void)
{
    char self[PATH_MAX], real[PATH_MAX], dir[PATH_MAX], game[PATH_MAX];
    uint32_t n = sizeof self;
    if (_NSGetExecutablePath(self, &n) != 0 || !realpath(self, real)) fail("cannot locate", "itself");
    snprintf(dir, sizeof dir, "%s", dirname(real));
    snprintf(game, sizeof game, "%s/%s", dir, LAUNCH_BIN);

    /* A self-contained app carries the ROMs (Resources/extracted) and the HD textures (Resources/HD) inside it, so it plays from
     * wherever it is moved. $<ROM_ENV> or the build's LAUNCH_ROMS point a development bundle at a ROM folder outside instead. */
    char res[PATH_MAX], roms[PATH_MAX];
    snprintf(res, sizeof res, "%s/../Resources", dir);
    const char *env = getenv(LAUNCH_ROM_ENV);
    struct stat st;
    snprintf(roms, sizeof roms, "%s/extracted", res);
    const int bundled = !(env && *env) && stat(roms, &st) == 0 && S_ISDIR(st.st_mode);
    const char *home = getenv("HOME");
    /* A release app (LAUNCH_PLAYER_ROMS) carries no game data: the player's own MAME zip, chosen once, is unpacked by the game
     * into ~/Library/Application Support/<game>/extracted (engine/romzip.c), and saves and settings sit beside it. */
    const int player = LAUNCH_PLAYER_ROMS && !bundled && !(env && *env) && home && *home;
    if (player) {
        char w[PATH_MAX];
        snprintf(w, sizeof w, "%s/Library", home); mkdir(w, 0755);
        snprintf(w, sizeof w, "%s/Library/Application Support", home); mkdir(w, 0755);
        snprintf(w, sizeof w, "%s/Library/Application Support/%s", home, LAUNCH_NAME); mkdir(w, 0755);
        snprintf(roms, sizeof roms, "%s/extracted", w); mkdir(roms, 0755);
        player_roms(w, home);
        setenv("ENG_ROM_HINT", "Open the game again and choose your MAME ROM set (" LAUNCH_ZIP_HELP ").", 1);   /* the game's own message */
    } else if (!bundled) snprintf(roms, sizeof roms, "%s", env && *env ? env : LAUNCH_ROMS);
    if (stat(roms, &st) != 0 || !S_ISDIR(st.st_mode)) fail("ROMs not found at", roms);

    /* the working folder holds what the player makes -- settings, saves, high scores: ~/Library/Application Support/<game> for a
     * self-contained app (the app itself is never written to), else the folder that holds the outside extracted/ */
    char work[PATH_MAX];
    if ((bundled || player) && home && *home) {
        snprintf(work, sizeof work, "%s/Library", home); mkdir(work, 0755);
        snprintf(work, sizeof work, "%s/Library/Application Support", home); mkdir(work, 0755);
        snprintf(work, sizeof work, "%s/Library/Application Support/%s", home, LAUNCH_NAME); mkdir(work, 0755);
    } else {
        char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s", roms); snprintf(work, sizeof work, "%s", dirname(tmp));
    }
    if (chdir(work) != 0) fail("cannot enter", work);
    setenv(LAUNCH_ROM_ENV, roms, 1);           /* Time Crisis II reads it; Time Crisis takes the folder as argv[1] */

    if (home && *home) {
        char logs[PATH_MAX], log[PATH_MAX];
        snprintf(logs, sizeof logs, "%s/Library", home); mkdir(logs, 0755);
        snprintf(logs, sizeof logs, "%s/Library/Logs", home); mkdir(logs, 0755);
        snprintf(logs, sizeof logs, "%s/Library/Logs/%s", home, LAUNCH_NAME);
        mkdir(logs, 0755);
        snprintf(log, sizeof log, "%s/last-run.log", logs);
        const int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd); }
    }
    if (bundled) fprintf(stderr, "[LAUNCH] self-contained: ROMs %s, saves and settings in %s\n", roms, work);
    if (player) fprintf(stderr, "[LAUNCH] release app: the player's ROMs in %s, saves and settings in %s\n", roms, work);

    setenv("SS22_AIMDBG", "1", 0);             /* the gun's aim, once a second, in the log: player reports can be checked */
    setenv("SDL_VIDEO_MAC_FULLSCREEN_SPACES", "0", 0);   /* Display > Borderless: the whole screen at once, no Space or animation */
    {
        char hd[PATH_MAX], sp[PATH_MAX];
        snprintf(hd, sizeof hd, "%s/HD", res);
        if (bundled || player) {
            /* HD textures (macos/tex_hd.h): the finished pack inside the app, read only -- nothing is collected or upscaled. A release
             * app has no pack (it would be game art); Time Crisis II's tutorial panels that say right click (hd-fixed) are still shown. */
            char fx[PATH_MAX]; snprintf(fx, sizeof fx, "%s/hd-fixed", res);
            if (stat(hd, &st) == 0 && S_ISDIR(st.st_mode)) setenv("ENG_TEX_HD", hd, 0);
            else if (player && stat(fx, &st) == 0 && S_ISDIR(st.st_mode)) setenv("ENG_TEX_HD", fx, 0);
        } else {
            /* a development bundle: the pack beside the saves; new textures are collected and upscaled at quit */
            char cwd[PATH_MAX], nw[PATH_MAX], up[PATH_MAX];
            if (getcwd(cwd, sizeof cwd)) {
                snprintf(hd, sizeof hd, "%s/HD", cwd); snprintf(nw, sizeof nw, "%s/HD-new", cwd);
                mkdir(hd, 0755); mkdir(nw, 0755);
                setenv("ENG_TEX_HD", hd, 0); setenv("ENG_TEX_DUMP", nw, 0);
                /* HD images the app ships (Resources/hd-fixed: Time Crisis II's tutorial panels saying right click) go into the folder
                 * every launch, over anything of the same name (the upscaler's version of the old panel) */
                char fx[PATH_MAX]; snprintf(fx, sizeof fx, "%s/hd-fixed", res);
                DIR *dd = opendir(fx);
                for (struct dirent *e; dd && (e = readdir(dd)); ) {
                    if (e->d_name[0] == '.' || !strstr(e->d_name, ".png")) continue;
                    char a[PATH_MAX], b[PATH_MAX]; snprintf(a, sizeof a, "%s/%s", fx, e->d_name); snprintf(b, sizeof b, "%s/%s", hd, e->d_name);
                    FILE *in = fopen(a, "rb"), *out = in ? fopen(b, "wb") : NULL;
                    char buf[65536]; size_t n;
                    while (in && out && (n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
                    if (in) fclose(in); if (out) fclose(out);
                }
                if (dd) closedir(dd);
            }
            snprintf(up, sizeof up, "%s/hd-upscale.sh", res);
            if (access(up, R_OK) == 0) setenv("ENG_TEX_UPSCALE_CMD", up, 0);
        }
        snprintf(sp, sizeof sp, "%s/sprites", res);   /* replacement sprites (engine/ss22_gl.c) */
        if (access(sp, R_OK) == 0) setenv("ENG_SPRITE_DIR", sp, 0);
    }

    char *argv[4]; int a = 0;
    argv[a++] = game;
    if (LAUNCH_ROMS_ARG) argv[a++] = roms;
    argv[a++] = "--window";                    /* the saved window mode and size */
    argv[a] = NULL;
    execv(game, argv);
    fail("cannot start", game);
}

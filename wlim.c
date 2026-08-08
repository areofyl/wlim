/*
 * wlim — vimium-like click hints for wayland
 *
 * walks the AT-SPI2 accessibility tree of all visible windows,
 * draws labeled hints over every clickable element using a
 * GTK4 + gtk4-layer-shell overlay, and clicks via uinput
 * when you type a hint. works on any wlroots-based compositor.
 *
 * build:
 *   make
 * or manually:
 *   gcc -O2 -o wlim wlim.c $(pkg-config --cflags --libs gtk4 atspi-2 gtk4-layer-shell-0) -lm
 */

#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <atspi/atspi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/uinput.h>
#include <linux/input-event-codes.h>

#define MAX_TARGETS  1024
#define MAX_LABEL    4
#define MAX_TYPED    8

/* ------------------------------------------------------------------ */
/*  configuration                                                      */
/* ------------------------------------------------------------------ */

static struct {
    char hint_bg[32];
    char hint_fg[32];
    char hint_fg_dim[32];
    char hint_border[32];
    int  hint_font_size;
    int  hint_border_radius;
    int  scroll_speed;      /* ticks per j/k press */
    int  page_speed;        /* ticks per d/u press */
    int  jump_speed;        /* ticks per G/gg */
    char bar_position[8];   /* "top", "bottom", or "none" */
} cfg = {
    .hint_bg           = "#2a2a2a",
    .hint_fg           = "#e0e0e0",
    .hint_fg_dim       = "#666",
    .hint_border       = "rgba(255,255,255,0.6)",
    .hint_font_size    = 11,
    .hint_border_radius = 4,
    .scroll_speed      = 1,
    .page_speed        = 10,
    .jump_speed        = 200,
    .bar_position      = "top",
};

static void cfg_set(const char *key, const char *val) {
    if (strcmp(key, "hint_bg") == 0) strncpy(cfg.hint_bg, val, sizeof(cfg.hint_bg) - 1);
    else if (strcmp(key, "hint_fg") == 0) strncpy(cfg.hint_fg, val, sizeof(cfg.hint_fg) - 1);
    else if (strcmp(key, "hint_fg_dim") == 0) strncpy(cfg.hint_fg_dim, val, sizeof(cfg.hint_fg_dim) - 1);
    else if (strcmp(key, "hint_border") == 0) strncpy(cfg.hint_border, val, sizeof(cfg.hint_border) - 1);
    else if (strcmp(key, "hint_font_size") == 0) cfg.hint_font_size = atoi(val);
    else if (strcmp(key, "hint_border_radius") == 0) cfg.hint_border_radius = atoi(val);
    else if (strcmp(key, "scroll_speed") == 0) cfg.scroll_speed = atoi(val);
    else if (strcmp(key, "page_speed") == 0) cfg.page_speed = atoi(val);
    else if (strcmp(key, "jump_speed") == 0) cfg.jump_speed = atoi(val);
    else if (strcmp(key, "bar_position") == 0) strncpy(cfg.bar_position, val, sizeof(cfg.bar_position) - 1);
}

static void cfg_load(void) {
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char path[512];

    if (xdg)
        snprintf(path, sizeof(path), "%s/wlim/config", xdg);
    else if (home)
        snprintf(path, sizeof(path), "%s/.config/wlim/config", home);
    else
        return;

    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* skip comments and empty lines */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;

        /* strip trailing newline */
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';

        /* split key=value */
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';

        /* trim key and value */
        char *key = p;
        char *val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        char *kend = eq - 1;
        while (kend > key && (*kend == ' ' || *kend == '\t')) *kend-- = '\0';

        cfg_set(key, val);
    }
    fclose(f);
    fprintf(stderr, "[wlim] loaded config from %s\n", path);
}

/* lookup table for clickable roles */
static gboolean clickable_lut[256];

static void init_clickable_lut(void) {
    static const AtspiRole roles[] = {
        ATSPI_ROLE_PUSH_BUTTON,  ATSPI_ROLE_TOGGLE_BUTTON,
        ATSPI_ROLE_CHECK_BOX,    ATSPI_ROLE_RADIO_BUTTON,
        ATSPI_ROLE_MENU_ITEM,    ATSPI_ROLE_LINK,
        ATSPI_ROLE_PAGE_TAB,     ATSPI_ROLE_COMBO_BOX,
        ATSPI_ROLE_ENTRY,        ATSPI_ROLE_SPIN_BUTTON,
        ATSPI_ROLE_SLIDER,       ATSPI_ROLE_ICON,
        ATSPI_ROLE_LIST_ITEM,    ATSPI_ROLE_TABLE_CELL,
        ATSPI_ROLE_TREE_ITEM,    ATSPI_ROLE_TOOL_BAR,
        ATSPI_ROLE_TEXT,          ATSPI_ROLE_DOCUMENT_WEB,
    };
    memset(clickable_lut, 0, sizeof(clickable_lut));
    for (size_t i = 0; i < sizeof(roles)/sizeof(roles[0]); i++)
        if ((int)roles[i] < 256) clickable_lut[(int)roles[i]] = TRUE;
}

typedef struct {
    int x, y, w, h;   /* element bounds from AT-SPI */
    int lx, ly;       /* label display position (top-left of element) */
    int cx, cy;       /* click position (center of element) */
    char label[MAX_LABEL + 1];
    char name[128];   /* element text from AT-SPI */
    AtspiAccessible *node;  /* kept alive for do_action */
} Target;

typedef struct {
    GtkApplication *app;
    GtkWidget      *win;
    GtkWidget      *fixed;
    Target  targets[MAX_TARGETS];
    int     n_targets;
    GtkWidget *hint_labels[MAX_TARGETS];
    char    typed[MAX_TYPED + 1];
    int     typed_len;
    int     click_x, click_y;
    int     click_button;  /* BTN_LEFT, BTN_RIGHT, or BTN_MIDDLE */
    gboolean should_click;
    AtspiAccessible *click_node;  /* for AT-SPI direct action */
    gboolean search_mode;
    char    search[64];
    int     search_len;
    GtkWidget *search_box;
    int     mon_x, mon_y, mon_w, mon_h;  /* target monitor bounds */
    int     scale;                         /* GDK scale factor */
    int     n_monitors;
    GtkWidget *extra_wins[8];  /* overlay windows on other monitors */
    int     atspi_win_w, atspi_win_h;      /* AT-SPI window size (for offset calc) */
    int     atspi_off_x, atspi_off_y;      /* detected window offset */
} State;


/* ------------------------------------------------------------------ */
/*  label generation                                                   */
/* ------------------------------------------------------------------ */

static void generate_labels(Target *t, int n) {
    if (n <= 26) {
        for (int i = 0; i < n; i++) {
            t[i].label[0] = 'a' + i;
            t[i].label[1] = '\0';
        }
        return;
    }
    int len = 1, p = 26;
    while (p < n) { len++; p *= 26; }
    for (int i = 0; i < n; i++) {
        int v = i;
        for (int j = len - 1; j >= 0; j--) {
            t[i].label[j] = 'a' + (v % 26);
            v /= 26;
        }
        t[i].label[len] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/*  at-spi tree walk                                                   */
/* ------------------------------------------------------------------ */

/* check if a target overlaps an existing one (nearly identical position) */
static gboolean is_duplicate(Target *out, int n, int x, int y) {
    for (int i = n - 1; i >= 0 && i >= n - 10; i--) {
        int dx = abs(out[i].x - x);
        int dy = abs(out[i].y - y);
        if (dx <= 4 && dy <= 4) return TRUE;
    }
    return FALSE;
}


static void walk(AtspiAccessible *node, Target *out, int *n, int depth) {
    if (!node || depth > 30 || *n >= MAX_TARGETS) return;

    GError *err = NULL;
    AtspiRole role = atspi_accessible_get_role(node, &err);
    if (err) { g_error_free(err); goto kids; }

    /* check visibility early — skip entire subtrees that aren't showing */
    if (depth > 0) {
        AtspiStateSet *ss = atspi_accessible_get_state_set(node);
        if (ss) {
            gboolean showing = atspi_state_set_contains(ss, ATSPI_STATE_SHOWING);
            g_object_unref(ss);
            if (!showing) return; /* prune entire subtree */
        }
    }

    if ((int)role < 256 && clickable_lut[(int)role]) {
        AtspiComponent *comp = atspi_accessible_get_component_iface(node);
        if (comp) {
            AtspiRect *ext = atspi_component_get_extents(
                comp, ATSPI_COORD_TYPE_SCREEN, &err);
            if (ext && !err) {
                if (ext->width > 0 && ext->height > 0 &&
                    !is_duplicate(out, *n, ext->x, ext->y)) {
                    Target *t = &out[*n];
                    t->x = ext->x; t->y = ext->y;
                    t->w = ext->width; t->h = ext->height;
                    gchar *nm = atspi_accessible_get_name(node, NULL);
                    if (nm) {
                        strncpy(t->name, nm, sizeof(t->name) - 1);
                        t->name[sizeof(t->name) - 1] = '\0';
                        g_free(nm);
                    } else {
                        t->name[0] = '\0';
                    }
                    t->node = g_object_ref(node);
                    (*n)++;
                }
            }
            if (ext) g_free(ext);
            if (err) g_error_free(err);
            g_object_unref(comp);
        }
    }

kids:;
    int nc = atspi_accessible_get_child_count(node, NULL);
    for (int i = 0; i < nc && *n < MAX_TARGETS; i++) {
        AtspiAccessible *ch = atspi_accessible_get_child_at_index(node, i, NULL);
        if (ch) { walk(ch, out, n, depth + 1); g_object_unref(ch); }
    }
}

/* filter targets to only those whose click point falls within the given rect */
static void filter_targets_to_rect(State *st, int rx, int ry, int rw, int rh) {
    int out = 0;
    for (int i = 0; i < st->n_targets; i++) {
        int cx = st->targets[i].cx;
        int cy = st->targets[i].cy;
        if (cx >= rx && cx < rx + rw && cy >= ry && cy < ry + rh) {
            if (out != i) st->targets[out] = st->targets[i];
            out++;
        }
    }
    fprintf(stderr, "[wlim] filtered %d -> %d targets for monitor (%d,%d %dx%d)\n",
            st->n_targets, out, rx, ry, rw, rh);
    st->n_targets = out;
}

/* walk all AT-SPI apps/windows, collecting targets from every one.
 * uses AT-SPI window extents to detect and fix window-relative coords. */
static void collect_all_targets(State *st) {
    AtspiAccessible *desktop = atspi_get_desktop(0);
    int napps = atspi_accessible_get_child_count(desktop, NULL);

    for (int i = 0; i < napps && st->n_targets < MAX_TARGETS; i++) {
        AtspiAccessible *app = atspi_accessible_get_child_at_index(desktop, i, NULL);
        if (!app) continue;

        int nwins = atspi_accessible_get_child_count(app, NULL);

        for (int k = 0; k < nwins && st->n_targets < MAX_TARGETS; k++) {
            AtspiAccessible *w = atspi_accessible_get_child_at_index(app, k, NULL);
            if (!w) continue;

            int start = st->n_targets;
            walk(w, st->targets, &st->n_targets, 0);
            int count = st->n_targets - start;

            if (count > 0) {
                gchar *dbg_name = atspi_accessible_get_name(w, NULL);

                /* check if coords are broken (GTK4 reports all zeros) */
                int zeros = 0;
                for (int t = start; t < st->n_targets; t++)
                    if (st->targets[t].x == 0 && st->targets[t].y == 0) zeros++;

                if ((double)zeros / count >= 0.8) {
                    fprintf(stderr, "[wlim] window \"%s\": %d targets with broken coords, skipping\n",
                            dbg_name ? dbg_name : "?", count);
                    st->n_targets = start;
                } else {
                    int off_x = 0, off_y = 0;
                    /* store AT-SPI window size for pointer-based offset detection */
                    AtspiComponent *wcomp = atspi_accessible_get_component_iface(w);
                    if (wcomp) {
                        AtspiRect *wext = atspi_component_get_extents(
                            wcomp, ATSPI_COORD_TYPE_SCREEN, NULL);
                        if (wext) {
                            st->atspi_win_w = wext->width;
                            st->atspi_win_h = wext->height;
                            off_x = wext->x;
                            off_y = wext->y;
                            fprintf(stderr, "[wlim] window \"%s\": %d targets, "
                                    "win=(%d,%d %dx%d)\n",
                                    dbg_name ? dbg_name : "?", count,
                                    wext->x, wext->y, wext->width, wext->height);
                            g_free(wext);
                        }
                        g_object_unref(wcomp);
                    }

                    /* apply offset to get screen-absolute click positions */
                    for (int t = start; t < st->n_targets; t++) {
                        Target *tg = &st->targets[t];
                        tg->cx = tg->x + off_x + tg->w / 2;
                        tg->cy = tg->y + off_y + tg->h / 2;
                    }
                }
                if (dbg_name) g_free(dbg_name);
            }
            g_object_unref(w);
        }
        g_object_unref(app);
    }
}

/* ------------------------------------------------------------------ */
/*  uinput — direct virtual input device                               */
/* ------------------------------------------------------------------ */

static int cached_sw = 1920, cached_sh = 1080;

static void emit(int fd, int type, int code, int val) {
    struct input_event ev = {0};
    ev.type = type;
    ev.code = code;
    ev.value = val;
    if (write(fd, &ev, sizeof(ev)) < 0)
        fprintf(stderr, "[wlim] write failed: %s\n", strerror(errno));
}

/* pre-created uinput device fd — created once at startup so
 * the compositor has already registered it by click time */
static int click_uinput_fd = -1;

static void click_uinput_create(int sw, int sh) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[wlim] cannot open /dev/uinput: %s\n", strerror(errno));
        return;
    }

    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);

    struct uinput_abs_setup abs_x = {0};
    abs_x.code = ABS_X;
    abs_x.absinfo.minimum = 0;
    abs_x.absinfo.maximum = sw - 1;
    ioctl(fd, UI_ABS_SETUP, &abs_x);

    struct uinput_abs_setup abs_y = {0};
    abs_y.code = ABS_Y;
    abs_y.absinfo.minimum = 0;
    abs_y.absinfo.maximum = sh - 1;
    ioctl(fd, UI_ABS_SETUP, &abs_y);

    struct uinput_setup setup = {0};
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "wlim-pointer");
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0x1234;
    setup.id.product = 0x5678;
    setup.id.version = 1;
    ioctl(fd, UI_DEV_SETUP, &setup);
    ioctl(fd, UI_DEV_CREATE);
    click_uinput_fd = fd;
}

static void click_uinput_destroy(void) {
    if (click_uinput_fd >= 0) {
        ioctl(click_uinput_fd, UI_DEV_DESTROY);
        close(click_uinput_fd);
        click_uinput_fd = -1;
    }
}

static gboolean do_atspi_action(AtspiAccessible *node) {
    AtspiAction *action = atspi_accessible_get_action_iface(node);
    if (action) {
        int n = atspi_action_get_n_actions(action, NULL);
        if (n > 0) {
            gboolean ok = atspi_action_do_action(action, 0, NULL);
            fprintf(stderr, "[wlim] atspi do_action(0) -> %s\n", ok ? "ok" : "fail");
            g_object_unref(action);
            return ok;
        }
        g_object_unref(action);
    }
    /* fallback: try grabbing focus */
    AtspiComponent *comp = atspi_accessible_get_component_iface(node);
    if (comp) {
        gboolean ok = atspi_component_grab_focus(comp, NULL);
        fprintf(stderr, "[wlim] atspi grab_focus -> %s\n", ok ? "ok" : "fail");
        g_object_unref(comp);
        return ok;
    }
    return FALSE;
}

static void do_click(int x, int y, int button) {
    int fd = click_uinput_fd;
    if (fd < 0) {
        fprintf(stderr, "[wlim] no uinput device\n");
        return;
    }

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= cached_sw) x = cached_sw - 1;
    if (y >= cached_sh) y = cached_sh - 1;

    const char *bname = button == BTN_RIGHT ? "right" : button == BTN_MIDDLE ? "middle" : "left";
    fprintf(stderr, "[wlim] %s-clicking at (%d,%d) screen=(%dx%d)\n", bname, x, y, cached_sw, cached_sh);

    /* move cursor to position */
    emit(fd, EV_ABS, ABS_X, x);
    emit(fd, EV_ABS, ABS_Y, y);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(16000); /* one frame at 60Hz */

    /* nudge to trigger wl_pointer.enter */
    emit(fd, EV_REL, REL_X, 1);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(8000);
    emit(fd, EV_REL, REL_X, -1);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(16000);

    /* click */
    emit(fd, EV_KEY, button, 1);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(8000);
    emit(fd, EV_KEY, button, 0);
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

/* ------------------------------------------------------------------ */
/*  scroll mode — evdev keyboard grab + uinput scroll                  */
/* ------------------------------------------------------------------ */

#include <signal.h>
#include <dirent.h>
#include <linux/input.h>

static volatile sig_atomic_t scroll_quit = 0;
static int scroll_kbd_fd = -1;  /* for signal handler cleanup */

static void scroll_sighandler(int sig) {
    (void)sig;
    /* release keyboard grab so user isn't stuck */
    if (scroll_kbd_fd >= 0) ioctl(scroll_kbd_fd, EVIOCGRAB, 0);
    scroll_quit = 1;
}

/* find the primary keyboard evdev device */
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define NBITS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(bit, arr) ((arr[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1)

static int find_keyboard(void) {
    DIR *d = opendir("/dev/input");
    if (!d) return -1;

    struct dirent *ent;
    int best_fd = -1;

    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;

        char path[280];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;

        /* check if this device has EV_KEY */
        unsigned long evbits[NBITS(EV_MAX + 1)] = {0};
        ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits);

        if (!TEST_BIT(EV_KEY, evbits)) {
            close(fd); continue;
        }

        /* check for real keyboard keys */
        unsigned long keybits[NBITS(KEY_MAX + 1)] = {0};
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);

        if (TEST_BIT(KEY_A, keybits) && TEST_BIT(KEY_J, keybits) &&
            TEST_BIT(KEY_ESC, keybits)) {
            /* skip our own virtual devices */
            char name[256] = {0};
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            if (strstr(name, "wlim")) { close(fd); continue; }

            if (best_fd >= 0) close(best_fd);
            best_fd = fd;
            fprintf(stderr, "[wlim] using keyboard: %s (%s)\n", path, name);
        } else {
            close(fd);
        }
    }
    closedir(d);
    return best_fd;
}

static int scroll_uinput_create(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[wlim] cannot open /dev/uinput: %s\n", strerror(errno));
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);

    struct uinput_setup setup = {0};
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "wlim-scroll");
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0x1234;
    setup.id.product = 0x5679;
    setup.id.version = 1;
    ioctl(fd, UI_DEV_SETUP, &setup);
    ioctl(fd, UI_DEV_CREATE);
    usleep(100000);
    return fd;
}

static void do_scroll(int fd, int vert, int horiz) {
    if (fd < 0) return;
    if (vert)  emit(fd, EV_REL, REL_WHEEL, vert);
    if (horiz) emit(fd, EV_REL, REL_HWHEEL, horiz);
    emit(fd, EV_SYN, SYN_REPORT, 0);
}

static int scroll_main(void) {
    int kbd = find_keyboard();
    if (kbd < 0) {
        fprintf(stderr, "[wlim] no keyboard found\n");
        (void)!system("notify-send -t 3000 wlim 'no keyboard found'");
        return 1;
    }

    int ufd = scroll_uinput_create();
    if (ufd < 0) { close(kbd); return 1; }

    /* wait for all modifier keys to be released before grabbing,
     * so the compositor sees the releases from the launch keybind */
    {
        static const int mods[] = {
            KEY_LEFTSHIFT, KEY_RIGHTSHIFT,
            KEY_LEFTCTRL, KEY_RIGHTCTRL,
            KEY_LEFTALT, KEY_RIGHTALT,
            KEY_LEFTMETA, KEY_RIGHTMETA,
        };
        for (int tries = 0; tries < 100; tries++) {
            unsigned long ks[NBITS(KEY_MAX + 1)] = {0};
            ioctl(kbd, EVIOCGKEY(sizeof(ks)), ks);
            int any = 0;
            for (size_t i = 0; i < sizeof(mods)/sizeof(mods[0]); i++)
                if (TEST_BIT(mods[i], ks)) { any = 1; break; }
            if (!any) break;
            usleep(10000);  /* 10ms */
        }
    }

    /* grab keyboard exclusively — all keys come to us */
    if (ioctl(kbd, EVIOCGRAB, 1) < 0) {
        fprintf(stderr, "[wlim] EVIOCGRAB failed: %s\n", strerror(errno));
        close(kbd);
        ioctl(ufd, UI_DEV_DESTROY); close(ufd);
        return 1;
    }

    scroll_kbd_fd = kbd;
    signal(SIGTERM, scroll_sighandler);
    signal(SIGINT, scroll_sighandler);

    fprintf(stderr, "[wlim] scroll mode active (Escape to exit)\n");

    int shift_held = 0;
    int awaiting_g = 0;
    struct input_event ev;

    while (!scroll_quit) {
        ssize_t n = read(kbd, &ev, sizeof(ev));
        if (n != sizeof(ev)) break;
        if (ev.type != EV_KEY) continue;

        /* track shift state */
        if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT) {
            shift_held = (ev.value != 0);  /* 1=press, 2=repeat, 0=release */
            continue;
        }

        /* only act on press (1) and repeat (2), not release (0) */
        if (ev.value == 0) continue;

        /* gg sequence */
        if (awaiting_g) {
            awaiting_g = 0;
            if (ev.code == KEY_G) {
                for (int i = 0; i < cfg.jump_speed; i++) do_scroll(ufd, -1, 0);
                continue;
            }
            /* not g — fall through */
        }

        switch (ev.code) {
            case KEY_ESC:
                scroll_quit = 1;
                break;
            case KEY_J:
            case KEY_DOWN:
                for (int i = 0; i < cfg.scroll_speed; i++) do_scroll(ufd, 1, 0);
                break;
            case KEY_K:
            case KEY_UP:
                for (int i = 0; i < cfg.scroll_speed; i++) do_scroll(ufd, -1, 0);
                break;
            case KEY_H:
            case KEY_LEFT:
                for (int i = 0; i < cfg.scroll_speed; i++) do_scroll(ufd, 0, 1);
                break;
            case KEY_L:
            case KEY_RIGHT:
                for (int i = 0; i < cfg.scroll_speed; i++) do_scroll(ufd, 0, -1);
                break;
            case KEY_D:
                for (int i = 0; i < cfg.page_speed; i++) do_scroll(ufd, 1, 0);
                break;
            case KEY_U:
                for (int i = 0; i < cfg.page_speed; i++) do_scroll(ufd, -1, 0);
                break;
            case KEY_G:
                if (shift_held) {
                    for (int i = 0; i < cfg.jump_speed; i++) do_scroll(ufd, 1, 0);
                } else {
                    awaiting_g = 1;
                }
                break;
        }
    }

    ioctl(kbd, EVIOCGRAB, 0);  /* release grab */
    close(kbd);
    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
    scroll_kbd_fd = -1;
    fprintf(stderr, "[wlim] scroll mode exited\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  hint mode — overlay                                                */
/* ------------------------------------------------------------------ */

static gboolean strcasestr_match(const char *hay, const char *needle) {
    if (!needle[0]) return TRUE;
    size_t nlen = strlen(needle);
    for (; *hay; hay++) {
        if (g_ascii_tolower(*hay) == g_ascii_tolower(needle[0]) &&
            g_ascii_strncasecmp(hay, needle, nlen) == 0)
            return TRUE;
    }
    return FALSE;
}

static void update_search_box(State *s) {
    char buf[80];
    snprintf(buf, sizeof(buf), "/ %s", s->search);
    gtk_label_set_text(GTK_LABEL(s->search_box), buf);
}

static void apply_search_filter(State *s) {
    /* hide hints that don't match search, show those that do */
    int visible = 0;
    for (int i = 0; i < s->n_targets; i++) {
        gboolean match = strcasestr_match(s->targets[i].name, s->search);
        gtk_widget_set_visible(s->hint_labels[i], match);
        if (match) visible++;
    }
    fprintf(stderr, "[wlim] search \"%s\": %d matches\n", s->search, visible);
}

static void relabel_visible(State *s) {
    /* re-generate short labels for only the visible (filtered) targets */
    int visible[MAX_TARGETS], nv = 0;
    for (int i = 0; i < s->n_targets; i++)
        if (gtk_widget_get_visible(s->hint_labels[i]))
            visible[nv++] = i;

    /* generate labels for nv items */
    Target tmp[MAX_TARGETS];
    for (int i = 0; i < nv; i++) tmp[i] = s->targets[visible[i]];
    generate_labels(tmp, nv);

    /* clear all labels first */
    for (int i = 0; i < s->n_targets; i++)
        s->targets[i].label[0] = '\0';

    /* assign new labels and update widgets */
    for (int i = 0; i < nv; i++) {
        int idx = visible[i];
        strcpy(s->targets[idx].label, tmp[i].label);
        gtk_label_set_text(GTK_LABEL(s->hint_labels[idx]), tmp[i].label);
    }
}

static void update_hints(State *s) {
    for (int i = 0; i < s->n_targets; i++) {
        const char *l = s->targets[i].label;
        if (strncmp(l, s->typed, s->typed_len) == 0) {
            gtk_widget_set_visible(s->hint_labels[i], TRUE);
            char mk[128], matched[MAX_LABEL+1] = {0};
            strncpy(matched, l, s->typed_len);
            snprintf(mk, sizeof(mk),
                "<span foreground=\"%s\">%s</span>"
                "<span foreground=\"%s\">%s</span>",
                cfg.hint_fg_dim, matched,
                cfg.hint_fg, l + s->typed_len);
            gtk_label_set_markup(GTK_LABEL(s->hint_labels[i]), mk);
        } else {
            gtk_widget_set_visible(s->hint_labels[i], FALSE);
        }
    }
}

static gboolean on_key(GtkEventControllerKey *ctrl, guint keyval,
                        guint keycode, GdkModifierType mod, gpointer data)
{
    State *s = data;
    const char *kn = gdk_keyval_name(keyval);

    if (g_strcmp0(kn, "Escape") == 0) {
        s->should_click = FALSE;
        g_application_quit(G_APPLICATION(s->app));
        return TRUE;
    }

    /* enter search mode with / */
    if (keyval == '/' && !s->search_mode) {
        s->search_mode = TRUE;
        s->search_len = 0;
        s->search[0] = '\0';
        gtk_widget_set_visible(s->search_box, TRUE);
        update_search_box(s);
        return TRUE;
    }

    /* search mode: type to filter, Enter to confirm, BackSpace to delete */
    if (s->search_mode) {
        if (g_strcmp0(kn, "Return") == 0) {
            s->search_mode = FALSE;
            gtk_widget_set_visible(s->search_box, FALSE);
            relabel_visible(s);
            s->typed_len = 0;
            s->typed[0] = '\0';
            return TRUE;
        }
        if (g_strcmp0(kn, "BackSpace") == 0) {
            if (s->search_len > 0) {
                s->search[--s->search_len] = '\0';
                apply_search_filter(s);
                update_search_box(s);
            }
            return TRUE;
        }
        char sch = 0;
        if (keyval >= 0x20 && keyval <= 0x7e) sch = (char)keyval;
        if (sch && s->search_len < (int)sizeof(s->search) - 1) {
            s->search[s->search_len++] = sch;
            s->search[s->search_len] = '\0';
            apply_search_filter(s);
            update_search_box(s);
        }
        return TRUE;
    }

    if (g_strcmp0(kn, "BackSpace") == 0) {
        if (s->typed_len > 0) { s->typed[--s->typed_len] = '\0'; update_hints(s); }
        return TRUE;
    }
    char ch = 0;
    if (keyval >= 'a' && keyval <= 'z') ch = (char)keyval;
    else if (keyval >= 'A' && keyval <= 'Z') ch = (char)(keyval + 32);
    if (!ch || s->typed_len >= MAX_TYPED) return TRUE;

    s->typed[s->typed_len++] = ch;
    s->typed[s->typed_len] = '\0';
    update_hints(s);

    int mi = -1, mc = 0;
    for (int i = 0; i < s->n_targets; i++)
        if (strcmp(s->targets[i].label, s->typed) == 0) { mi = i; mc++; }
    if (mc == 1) {
        s->should_click = TRUE;
        s->click_x = s->targets[mi].cx;
        s->click_y = s->targets[mi].cy;
        s->click_button = (mod & GDK_SHIFT_MASK) ? BTN_RIGHT
                        : (mod & GDK_CONTROL_MASK) ? BTN_MIDDLE
                        : BTN_LEFT;
        s->click_node = s->targets[mi].node;
        gtk_window_destroy(GTK_WINDOW(s->win));
        return TRUE;
    }

    int any = 0;
    for (int i = 0; i < s->n_targets; i++)
        if (strncmp(s->targets[i].label, s->typed, s->typed_len) == 0) any++;
    if (!any) { s->typed_len = 0; s->typed[0] = '\0'; update_hints(s); }

    return TRUE;
}

static void place_hints(State *s, int off_x, int off_y);
static void infer_offset(State *s);

/* called from idle after the overlay is mapped and sized.
 * we now know which monitor the compositor put it on. */
static gboolean on_layout_done(gpointer data) {
    State *s = data;
    int win_w = gtk_widget_get_width(s->win);
    int win_h = gtk_widget_get_height(s->win);

    /* figure out which monitor we actually landed on by matching size */
    GdkDisplay *disp = gdk_display_get_default();
    GListModel *mons = gdk_display_get_monitors(disp);
    guint nmons = g_list_model_get_n_items(mons);
    int max_x = 0, max_y = 0;

    s->mon_x = 0; s->mon_y = 0;
    s->mon_w = win_w; s->mon_h = win_h;
    s->scale = 1;

    for (guint i = 0; i < nmons; i++) {
        GdkMonitor *mon = g_list_model_get_item(mons, i);
        GdkRectangle geo;
        gdk_monitor_get_geometry(mon, &geo);
        int sc = gdk_monitor_get_scale_factor(mon);
        const char *conn = gdk_monitor_get_connector(mon);
        fprintf(stderr, "[wlim] GDK monitor %d: %s (%d,%d %dx%d) scale=%d\n",
                i, conn ? conn : "?",
                geo.x, geo.y, geo.width, geo.height, sc);
        if (geo.x + geo.width > max_x) max_x = geo.x + geo.width;
        if (geo.y + geo.height > max_y) max_y = geo.y + geo.height;

        /* match by size — the overlay fills whichever output it's on */
        if (geo.width == win_w && geo.height == win_h) {
            s->mon_x = geo.x;
            s->mon_y = geo.y;
            s->mon_w = geo.width;
            s->mon_h = geo.height;
            s->scale = sc;
        }
        g_object_unref(mon);
    }

    /* uinput needs physical pixel bounds; GDK geometry is logical */
    int sc = s->scale;
    if (max_x > 0) cached_sw = max_x * sc;
    if (max_y > 0) cached_sh = max_y * sc;
    click_uinput_create(cached_sw, cached_sh);

    fprintf(stderr, "[wlim] overlay mapped at %dx%d -> monitor (%d,%d %dx%d) scale=%d\n",
            win_w, win_h, s->mon_x, s->mon_y, s->mon_w, s->mon_h, sc);

    /* infer window offset from size difference */
    if (s->atspi_off_x == 0 && s->atspi_off_y == 0)
        infer_offset(s);

    place_hints(s, 0, 0);
    return G_SOURCE_REMOVE;
}

static void place_hints(State *s, int off_x, int off_y) {
    int sc = s->scale;
    /* apply offset to all targets */
    for (int i = 0; i < s->n_targets; i++) {
        s->targets[i].cx += off_x;
        s->targets[i].cy += off_y;
    }

    /* filter targets to only those on the monitor */
    filter_targets_to_rect(s, s->mon_x * sc, s->mon_y * sc,
                           s->mon_w * sc, s->mon_h * sc);
    generate_labels(s->targets, s->n_targets);

    for (int i = 0; i < s->n_targets; i++) {
        Target *t = &s->targets[i];
        t->lx = (t->cx - t->w / 2) / sc - s->mon_x;
        t->ly = (t->cy - t->h / 2) / sc - s->mon_y;

        if (t->lx < 0) t->lx = 0;
        if (t->ly < 0) t->ly = 0;
        if (t->lx > s->mon_w - 20) t->lx = s->mon_w - 20;
        if (t->ly > s->mon_h - 14) t->ly = s->mon_h - 14;

        if (i < 10)
            fprintf(stderr, "[wlim] hint '%s' (%s): cx,cy=(%d,%d) placed=(%d,%d)\n",
                    t->label, t->name, t->cx, t->cy, t->lx, t->ly);

        GtkWidget *lbl = gtk_label_new(t->label);
        gtk_widget_add_css_class(lbl, "hint-label");
        gtk_fixed_put(GTK_FIXED(s->fixed), lbl, t->lx, t->ly);
        s->hint_labels[i] = lbl;
    }
    fprintf(stderr, "[wlim] placed %d hints (offset %d,%d)\n", s->n_targets, off_x, off_y);
}

/* infer window position from AT-SPI window size vs monitor size.
 * on wayland, apps report (0,0) as their position. but for tiling WMs,
 * we can figure out the real offset from the gap between window and monitor. */
static void infer_offset(State *s) {
    if (s->atspi_win_w <= 0 || s->atspi_win_h <= 0) return;
    int sc = s->scale;
    int mw = s->mon_w * sc, mh = s->mon_h * sc;
    int ww = s->atspi_win_w, wh = s->atspi_win_h;

    /* horizontal: assume symmetric gaps */
    int gap_x = (mw - ww) / 2;
    int gap_total_y = mh - wh;
    int off_x = gap_x;
    int off_y = gap_x;  /* default: just the gap */

    int bar_h = gap_total_y - 2 * gap_x;
    if (bar_h < 0) bar_h = 0;

    if (strcmp(cfg.bar_position, "top") == 0)
        off_y = bar_h + gap_x;
    else if (strcmp(cfg.bar_position, "bottom") == 0)
        off_y = gap_x;
    else if (strcmp(cfg.bar_position, "none") == 0)
        off_y = gap_total_y / 2;

    fprintf(stderr, "[wlim] inferred offset: (%d,%d) from win=%dx%d mon=%dx%d\n",
            off_x, off_y, ww, wh, mw, mh);

    for (int i = 0; i < s->n_targets; i++) {
        s->targets[i].cx += off_x;
        s->targets[i].cy += off_y;
    }
    s->atspi_off_x = off_x;
    s->atspi_off_y = off_y;
}

static void on_activate(GtkApplication *app, gpointer data) {
    State *s = data;
    s->app = app;
    GtkWidget *win = gtk_application_window_new(app);
    s->win = win;

    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(GTK_WINDOW(win), "wlim");
    gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);

    /* pick the monitor that contains the most targets */
    {
        GdkDisplay *disp = gdk_display_get_default();
        GListModel *mons = gdk_display_get_monitors(disp);
        guint nmons = g_list_model_get_n_items(mons);
        GdkMonitor *best_mon = NULL;
        int best_count = 0;
        for (guint i = 0; i < nmons; i++) {
            GdkMonitor *mon = g_list_model_get_item(mons, i);
            GdkRectangle geo;
            gdk_monitor_get_geometry(mon, &geo);
            int sc = gdk_monitor_get_scale_factor(mon);
            /* AT-SPI coords are physical, geo is logical — scale geo up */
            int rx = geo.x * sc, ry = geo.y * sc;
            int rw = geo.width * sc, rh = geo.height * sc;
            int count = 0;
            for (int t = 0; t < s->n_targets; t++) {
                int cx = s->targets[t].cx, cy = s->targets[t].cy;
                if (cx >= rx && cx < rx + rw && cy >= ry && cy < ry + rh)
                    count++;
            }
            fprintf(stderr, "[wlim] monitor %d has %d targets\n", i, count);
            if (count > best_count) { best_count = count; best_mon = mon; }
            else g_object_unref(mon);
        }
        if (best_mon) {
            gtk_layer_set_monitor(GTK_WINDOW(win), best_mon);
            g_object_unref(best_mon);
        }
    }

    GtkCssProvider *css = gtk_css_provider_new();
    char cssbuf[1024];
    snprintf(cssbuf, sizeof(cssbuf),
        "window { background: rgba(0,0,0,0.01); }\n"
        ".hint-label {\n"
        "  background: %s;\n"
        "  color: %s;\n"
        "  font-size: %dpx;\n"
        "  font-weight: bold;\n"
        "  font-family: monospace;\n"
        "  padding: 1px 6px;\n"
        "  border-radius: %dpx;\n"
        "  border: 1px solid %s;\n"
        "}\n"
        ".search-box {\n"
        "  background: #1a1a1a;\n"
        "  color: %s;\n"
        "  font-size: 14px;\n"
        "  font-family: monospace;\n"
        "  padding: 6px 14px;\n"
        "  border-radius: 6px;\n"
        "  border: 1px solid #444;\n"
        "  margin-bottom: 40px;\n"
        "}\n",
        cfg.hint_bg, cfg.hint_fg, cfg.hint_font_size,
        cfg.hint_border_radius, cfg.hint_border, cfg.hint_fg);
    gtk_css_provider_load_from_string(css, cssbuf);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(css);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_window_set_child(GTK_WINDOW(win), overlay);

    GtkWidget *fixed = gtk_fixed_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), fixed);
    s->fixed = fixed;

    /* search box — centered at bottom, hidden by default */
    GtkWidget *search_box = gtk_label_new("/ ");
    gtk_widget_add_css_class(search_box, "search-box");
    gtk_widget_set_halign(search_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(search_box, GTK_ALIGN_END);
    gtk_widget_set_visible(search_box, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), search_box);
    s->search_box = search_box;

    g_idle_add(on_layout_done, s);

    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), s);
    gtk_widget_add_controller(win, kc);
    gtk_window_present(GTK_WINDOW(win));
}

static void on_shutdown(GtkApplication *app, gpointer data) {
    (void)app; (void)data;
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    cfg_load();

    /* check for flags */
    gboolean scroll_mode = FALSE;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scroll") == 0) scroll_mode = TRUE;
    }

    if (scroll_mode) return scroll_main();

    /* hint mode */
    init_clickable_lut();
    atspi_init();

    /* debug: show what AT-SPI can see */
    {
        AtspiAccessible *desktop = atspi_get_desktop(0);
        int napps = atspi_accessible_get_child_count(desktop, NULL);
        fprintf(stderr, "[wlim] AT-SPI desktop has %d apps\n", napps);
        for (int i = 0; i < napps; i++) {
            AtspiAccessible *app = atspi_accessible_get_child_at_index(desktop, i, NULL);
            if (!app) continue;
            gchar *name = atspi_accessible_get_name(app, NULL);
            int nwins = atspi_accessible_get_child_count(app, NULL);
            fprintf(stderr, "[wlim]   app %d: \"%s\" (%d windows)\n",
                    i, name ? name : "?", nwins);
            if (name) g_free(name);
            g_object_unref(app);
        }
    }

    State st = {0};
    collect_all_targets(&st);
    fprintf(stderr, "[wlim] collected %d targets total\n", st.n_targets);

    if (st.n_targets == 0) {
        fprintf(stderr, "[wlim] no clickable elements found\n");
        return 1;
    }

    generate_labels(st.targets, st.n_targets);

    GtkApplication *app = gtk_application_new("dev.wlim.overlay", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &st);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), &st);
    g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);

    /* click after GTK is fully torn down so the overlay is gone */
    if (st.should_click) {
        usleep(50000);
        do_click(st.click_x, st.click_y, st.click_button);
    }
    /* clean up AT-SPI refs */
    for (int i = 0; i < st.n_targets; i++)
        if (st.targets[i].node) g_object_unref(st.targets[i].node);
    click_uinput_destroy();
    return 0;
}

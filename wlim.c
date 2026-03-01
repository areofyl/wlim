/*
 * wlim — vimium-like click hints for wayland (hyprland)
 *
 * walks the AT-SPI2 accessibility tree of all visible windows,
 * draws labeled hints over every clickable element using a
 * GTK4 + gtk4-layer-shell overlay, and clicks via uinput
 * when you type a hint.
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
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/uinput.h>
#include <linux/input-event-codes.h>

#define MAX_TARGETS  1024
#define MAX_LABEL    4
#define MAX_TYPED    8

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
} State;

/* ------------------------------------------------------------------ */
/*  hyprctl — direct socket                                            */
/* ------------------------------------------------------------------ */

static char *hyprctl_request(const char *request) {
    const char *his = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!his) return NULL;

    char path[256];
    snprintf(path, sizeof(path), "/tmp/hypr/%s/.socket.sock", his);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        const char *xrd = getenv("XDG_RUNTIME_DIR");
        if (xrd) {
            snprintf(path, sizeof(path), "%s/hypr/%s/.socket.sock", xrd, his);
            strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                close(fd);
                return NULL;
            }
        } else {
            close(fd);
            return NULL;
        }
    }

    size_t rlen = strlen(request);
    if (write(fd, request, rlen) != (ssize_t)rlen) {
        close(fd);
        return NULL;
    }

    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    while (1) {
        ssize_t n = read(fd, buf + len, cap - len - 1);
        if (n <= 0) break;
        len += n;
        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
    }
    buf[len] = '\0';
    close(fd);
    return buf;
}

/* ------------------------------------------------------------------ */
/*  json helpers                                                       */
/* ------------------------------------------------------------------ */

static int json_int(const char *j, const char *key, int def) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(j, pat);
    if (!p) return def;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

static char *json_str(const char *j, const char *key, char *buf, size_t sz) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(j, pat);
    buf[0] = '\0';
    if (!p) return buf;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return buf;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < sz - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case '"': case '\\': case '/': buf[i++] = *p; break;
                case 'n': buf[i++] = '\n'; break;
                case 't': buf[i++] = '\t'; break;
                default:  buf[i++] = *p; break;
            }
            p++;
        } else {
            buf[i++] = *p++;
        }
    }
    buf[i] = '\0';
    return buf;
}

static void json_int_pair(const char *j, const char *key, int *a, int *b) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(j, pat);
    if (!p) { *a = 0; *b = 0; return; }
    p += strlen(pat);
    while (*p && *p != '[') p++;
    if (*p == '[') p++;
    *a = atoi(p);
    while (*p && *p != ',') p++;
    if (*p == ',') p++;
    *b = atoi(p);
}

/* ------------------------------------------------------------------ */
/*  hyprctl client geometry lookup                                     */
/* ------------------------------------------------------------------ */

/* find the matching '}' for a '{', handling nested braces */
static const char *find_block_end(const char *p) {
    int depth = 0;
    gboolean in_str = FALSE;
    for (; *p; p++) {
        if (*p == '\\' && in_str) { p++; continue; }
        if (*p == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) return p; }
    }
    return NULL;
}

/* find the geometry of a hyprland client by matching its pid against
 * the clients json array. returns false if not found. */
static gboolean find_client_geom(const char *clients_json, int pid,
                                  int *cx, int *cy, int *cw, int *ch)
{
    if (!clients_json || pid <= 0) return FALSE;

    const char *p = clients_json;
    while ((p = strchr(p, '{')) != NULL) {
        const char *end = find_block_end(p);
        if (!end) break;

        size_t blen = end - p + 1;
        char *block = malloc(blen + 1);
        memcpy(block, p, blen);
        block[blen] = '\0';

        int cpid = json_int(block, "pid", -1);
        if (cpid == pid) {
            json_int_pair(block, "at", cx, cy);
            json_int_pair(block, "size", cw, ch);
            free(block);
            return TRUE;
        }
        free(block);
        p = end + 1;
    }
    return FALSE;
}

/* check if two titles share a long enough common substring to be
 * considered the same window (handles " - Audio playing" etc) */
static gboolean titles_match(const char *a, const char *b) {
    if (!a[0] || !b[0]) return FALSE;
    if (strstr(a, b) || strstr(b, a)) return TRUE;
    /* check if one title starts with the other's first N chars.
     * chromium titles differ by suffixes like " - Audio playing" */
    size_t la = strlen(a), lb = strlen(b);
    size_t min = la < lb ? la : lb;
    if (min > 10 && strncmp(a, b, min) == 0) return TRUE;
    /* check shared prefix of at least 20 chars */
    size_t common = 0;
    while (common < la && common < lb && a[common] == b[common]) common++;
    if (common >= 20) return TRUE;
    return FALSE;
}

/* find client geometry by matching AT-SPI window title against
 * hyprctl client titles */
static gboolean find_client_geom_by_title(const char *clients_json,
                                            const char *title,
                                            int *cx, int *cy, int *cw, int *ch)
{
    if (!clients_json || !title || !title[0]) return FALSE;

    const char *p = clients_json;
    while ((p = strchr(p, '{')) != NULL) {
        const char *end = find_block_end(p);
        if (!end) break;

        size_t blen = end - p + 1;
        char *block = malloc(blen + 1);
        memcpy(block, p, blen);
        block[blen] = '\0';

        char ctitle[256];
        json_str(block, "title", ctitle, sizeof(ctitle));

        if (titles_match(ctitle, title)) {
            json_int_pair(block, "at", cx, cy);
            json_int_pair(block, "size", cw, ch);
            free(block);
            return TRUE;
        }
        free(block);
        p = end + 1;
    }
    return FALSE;
}

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

    if (depth > 0) {
        AtspiStateSet *ss = atspi_accessible_get_state_set(node);
        if (ss) {
            gboolean ok = atspi_state_set_contains(ss, ATSPI_STATE_VISIBLE)
                       && atspi_state_set_contains(ss, ATSPI_STATE_SHOWING);
            g_object_unref(ss);
            if (!ok) return;
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

/* walk all AT-SPI apps/windows, collecting targets from every one.
 * for windows with broken coords (GTK4), use grid_fallback per window. */
static void collect_all_targets(State *st, const char *clients_json) {
    AtspiAccessible *desktop = atspi_get_desktop(0);
    int napps = atspi_accessible_get_child_count(desktop, NULL);

    for (int i = 0; i < napps && st->n_targets < MAX_TARGETS; i++) {
        AtspiAccessible *app = atspi_accessible_get_child_at_index(desktop, i, NULL);
        if (!app) continue;

        int nwins = atspi_accessible_get_child_count(app, NULL);
        guint app_pid = atspi_accessible_get_process_id(app, NULL);

        for (int k = 0; k < nwins && st->n_targets < MAX_TARGETS; k++) {
            AtspiAccessible *w = atspi_accessible_get_child_at_index(app, k, NULL);
            if (!w) continue;

            /* remember where this window's targets start */
            int start = st->n_targets;

            walk(w, st->targets, &st->n_targets, 0);

            int count = st->n_targets - start;
            if (count > 0) {
                /* look up this window's actual geometry from hyprctl */
                int wx = 0, wy = 0, ww = 0, wh = 0;
                gboolean found = find_client_geom(clients_json,
                                                   (int)app_pid,
                                                   &wx, &wy, &ww, &wh);
                if (!found) {
                    gchar *wtitle = atspi_accessible_get_name(w, NULL);
                    if (wtitle) {
                        found = find_client_geom_by_title(clients_json,
                                    wtitle, &wx, &wy, &ww, &wh);
                        g_free(wtitle);
                    }
                }

                gchar *dbg_name = atspi_accessible_get_name(w, NULL);
                fprintf(stderr, "[wlim] window \"%s\": %d targets, geom found=%d at=(%d,%d) size=(%d,%d) pid_atspi=%d\n",
                        dbg_name ? dbg_name : "?", count, found, wx, wy, ww, wh, (int)app_pid);
                if (dbg_name) g_free(dbg_name);

                /* check if this window's coords are usable */
                int zeros = 0;
                for (int t = start; t < st->n_targets; t++)
                    if (st->targets[t].x == 0 && st->targets[t].y == 0) zeros++;

                if ((double)zeros / count >= 0.8) {
                    /* broken coords (GTK4) — distribute in a grid */
                    if (found && ww > 0 && wh > 0) {
                        int m = 30;
                        int gx = wx + m, gy = wy + m;
                        int gw = ww - m*2, gh = wh - m*2;
                        int cols = (int)ceil(sqrt((double)count));
                        int rows = (int)ceil((double)count / cols);
                        double cw = (double)gw / (cols ? cols : 1);
                        double ch = (double)gh / (rows ? rows : 1);
                        for (int t = 0; t < count; t++) {
                            int px = (int)(gx + (t % cols) * cw + cw / 2);
                            int py = (int)(gy + (t / cols) * ch + ch / 2);
                            st->targets[start + t].lx = px;
                            st->targets[start + t].ly = py;
                            st->targets[start + t].cx = px;
                            st->targets[start + t].cy = py;
                        }
                    } else {
                        st->n_targets = start;
                    }
                } else {
                    /* coords are present — check if they're window-relative.
                     * on wayland, some apps (chromium) report AT-SPI coords
                     * relative to the window instead of the screen. detect
                     * this by checking if all coords fall within [0, ww) x
                     * [0, wh) rather than [wx, wx+ww) x [wy, wy+wh). */
                    int off_x = 0, off_y = 0;
                    if (found && ww > 0 && wh > 0 && (wx > 0 || wy > 0)) {
                        int window_rel = 0;
                        for (int t = start; t < st->n_targets; t++) {
                            Target *tg = &st->targets[t];
                            if (tg->x >= 0 && tg->x < ww &&
                                tg->y >= 0 && tg->y < wh)
                                window_rel++;
                        }
                        /* if most coords fit inside [0,ww)x[0,wh) but the
                         * window isn't at (0,0), they're window-relative */
                        fprintf(stderr, "[wlim]   window_rel=%d/%d (%.0f%%)\n",
                                window_rel, count, 100.0 * window_rel / count);
                        if ((double)window_rel / count >= 0.8) {
                            off_x = wx;
                            off_y = wy;
                            fprintf(stderr, "[wlim]   applying offset (%d,%d)\n", off_x, off_y);
                        }
                    }

                    for (int t = start; t < st->n_targets; t++) {
                        Target *tg = &st->targets[t];
                        tg->cx = tg->x + off_x + tg->w / 2;
                        tg->cy = tg->y + off_y + tg->h / 2;
                        tg->lx = tg->x + off_x + 16;
                        tg->ly = tg->y + off_y + 8;
                    }
                }
            }

            g_object_unref(w);
        }
        g_object_unref(app);
    }
}

/* ------------------------------------------------------------------ */
/*  uinput — direct virtual input device                               */
/* ------------------------------------------------------------------ */

/* get the total screen bounding box from hyprctl monitors.
 * for multi-monitor setups this returns the combined extent. */
static void get_screen_bounds(int *total_w, int *total_h) {
    char *json = hyprctl_request("j/monitors");
    *total_w = 1920;
    *total_h = 1080;
    if (!json) return;

    int max_x = 0, max_y = 0;
    const char *p = json;
    while ((p = strchr(p, '{')) != NULL) {
        const char *end = find_block_end(p);
        if (!end) break;

        size_t blen = end - p + 1;
        char *block = malloc(blen + 1);
        memcpy(block, p, blen);
        block[blen] = '\0';

        int mx = json_int(block, "x", 0);
        int my = json_int(block, "y", 0);
        int mw = json_int(block, "width", 0);
        int mh = json_int(block, "height", 0);

        if (mx + mw > max_x) max_x = mx + mw;
        if (my + mh > max_y) max_y = my + mh;

        free(block);
        p = end + 1;
    }
    free(json);

    if (max_x > 0) *total_w = max_x;
    if (max_y > 0) *total_h = max_y;
}

static void emit(int fd, int type, int code, int val) {
    struct input_event ev = {0};
    ev.type = type;
    ev.code = code;
    ev.value = val;
    write(fd, &ev, sizeof(ev));
}

static void do_click(int x, int y, int button) {
    int sw, sh;
    get_screen_bounds(&sw, &sh);

    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[wlim] cannot open /dev/uinput: %s\n", strerror(errno));
        return;
    }

    /* enable event types */
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_ABSBIT, ABS_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_Y);
    ioctl(fd, UI_SET_KEYBIT, button);

    /* configure abs axes to match screen pixel dimensions */
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

    /* create the device */
    struct uinput_setup setup = {0};
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "wlim-pointer");
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor  = 0x1234;
    setup.id.product = 0x5678;
    setup.id.version = 1;
    ioctl(fd, UI_DEV_SETUP, &setup);
    ioctl(fd, UI_DEV_CREATE);

    /* small delay for compositor to register the new device */
    usleep(50000);

    /* clamp coordinates */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= sw) x = sw - 1;
    if (y >= sh) y = sh - 1;

    const char *bname = button == BTN_RIGHT ? "right" : button == BTN_MIDDLE ? "middle" : "left";
    fprintf(stderr, "[wlim] %s-clicking at (%d,%d) screen=(%dx%d)\n", bname, x, y, sw, sh);

    /* move to position */
    emit(fd, EV_ABS, ABS_X, x);
    emit(fd, EV_ABS, ABS_Y, y);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(20000);

    /* press */
    emit(fd, EV_KEY, button, 1);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(20000);

    /* release */
    emit(fd, EV_KEY, button, 0);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(20000);

    /* destroy */
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
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

        char path[128];
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
        system("notify-send -t 3000 wlim 'no keyboard found'");
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
                for (int i = 0; i < 200; i++) do_scroll(ufd, -1, 0);
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
                do_scroll(ufd, 1, 0);
                break;
            case KEY_K:
            case KEY_UP:
                do_scroll(ufd, -1, 0);
                break;
            case KEY_H:
            case KEY_LEFT:
                do_scroll(ufd, 0, 1);
                break;
            case KEY_L:
            case KEY_RIGHT:
                do_scroll(ufd, 0, -1);
                break;
            case KEY_D:
                for (int i = 0; i < 10; i++) do_scroll(ufd, 1, 0);
                break;
            case KEY_U:
                for (int i = 0; i < 10; i++) do_scroll(ufd, -1, 0);
                break;
            case KEY_G:
                if (shift_held) {
                    for (int i = 0; i < 200; i++) do_scroll(ufd, 1, 0);
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

static void update_hints(State *s) {
    for (int i = 0; i < s->n_targets; i++) {
        const char *l = s->targets[i].label;
        if (strncmp(l, s->typed, s->typed_len) == 0) {
            gtk_widget_set_visible(s->hint_labels[i], TRUE);
            char mk[128], matched[MAX_LABEL+1] = {0};
            strncpy(matched, l, s->typed_len);
            snprintf(mk, sizeof(mk),
                "<span foreground=\"#665600\">%s</span>"
                "<span foreground=\"#FFD600\">%s</span>",
                matched, l + s->typed_len);
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
        gtk_window_destroy(GTK_WINDOW(s->win));
        return TRUE;
    }

    int any = 0;
    for (int i = 0; i < s->n_targets; i++)
        if (strncmp(s->targets[i].label, s->typed, s->typed_len) == 0) any++;
    if (!any) { s->typed_len = 0; s->typed[0] = '\0'; update_hints(s); }

    return TRUE;
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

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window { background: rgba(0,0,0,0.01); }\n"
        ".hint-label {\n"
        "  background: rgba(30,30,30,0.92);\n"
        "  color: #FFD600;\n"
        "  font-size: 13px;\n"
        "  font-weight: bold;\n"
        "  font-family: monospace;\n"
        "  padding: 1px 5px;\n"
        "  border-radius: 3px;\n"
        "  border: 1px solid rgba(255,214,0,0.5);\n"
        "}\n");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_USER);
    g_object_unref(css);

    GtkWidget *fixed = gtk_fixed_new();
    gtk_window_set_child(GTK_WINDOW(win), fixed);
    s->fixed = fixed;

    for (int i = 0; i < s->n_targets; i++) {
        GtkWidget *lbl = gtk_label_new(s->targets[i].label);
        gtk_widget_add_css_class(lbl, "hint-label");
        gtk_fixed_put(GTK_FIXED(fixed), lbl, s->targets[i].lx, s->targets[i].ly);
        s->hint_labels[i] = lbl;
    }

    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), s);
    gtk_widget_add_controller(win, kc);
    gtk_window_present(GTK_WINDOW(win));
}

static void on_shutdown(GtkApplication *app, gpointer data) {
    State *s = data;
    if (s->should_click) {
        usleep(150000);
        do_click(s->click_x, s->click_y, s->click_button);
    }
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    /* check for --scroll mode */
    gboolean scroll_mode = FALSE;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scroll") == 0) scroll_mode = TRUE;
    }

    if (scroll_mode) return scroll_main();

    /* hint mode */
    init_clickable_lut();
    atspi_init();

    char *clients_json = hyprctl_request("j/clients");

    State st = {0};
    collect_all_targets(&st, clients_json);
    free(clients_json);

    if (st.n_targets == 0) {
        system("notify-send -t 3000 wlim 'no clickable elements found'");
        return 1;
    }

    generate_labels(st.targets, st.n_targets);

    GtkApplication *app = gtk_application_new("dev.wlim.overlay", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &st);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), &st);
    g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);
    return 0;
}

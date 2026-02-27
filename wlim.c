/*
 * wlim — vimium-like click hints for wayland (hyprland)
 *
 * walks the AT-SPI2 accessibility tree of all visible windows,
 * draws labeled hints over every clickable element using a
 * GTK4 + gtk4-layer-shell overlay, and clicks via ydotool
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
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <errno.h>

#define MAX_TARGETS  1024
#define MAX_LABEL    4
#define MAX_TYPED    8

/* ydotool absolute coordinates on hyprland don't map 1:1 to screen
 * pixels. this ratio was determined experimentally — your setup may
 * differ if you change monitor config or scale. */
#define YDOTOOL_RATIO 2.375

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
    int x, y, w, h;
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
    while (*p && *p != '"' && i < sz - 1) buf[i++] = *p++;
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

/* find the geometry of a hyprland client by matching its pid against
 * the clients json array. returns false if not found. */
static gboolean find_client_geom(const char *clients_json, int pid,
                                  int *cx, int *cy, int *cw, int *ch)
{
    if (!clients_json || pid <= 0) return FALSE;

    /* scan through the json array looking for objects with matching pid.
     * we look for each '{' block and check the pid inside it. */
    const char *p = clients_json;
    while ((p = strchr(p, '{')) != NULL) {
        /* find the end of this object */
        const char *end = strchr(p, '}');
        if (!end) break;

        /* extract pid from this object block */
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

/* find client geometry by matching AT-SPI window title against
 * hyprctl client titles */
static gboolean find_client_geom_by_title(const char *clients_json,
                                            const char *title,
                                            int *cx, int *cy, int *cw, int *ch)
{
    if (!clients_json || !title || !title[0]) return FALSE;

    const char *p = clients_json;
    while ((p = strchr(p, '{')) != NULL) {
        const char *end = strchr(p, '}');
        if (!end) break;

        size_t blen = end - p + 1;
        char *block = malloc(blen + 1);
        memcpy(block, p, blen);
        block[blen] = '\0';

        char ctitle[256];
        json_str(block, "title", ctitle, sizeof(ctitle));

        /* partial match — at-spi titles are often truncated */
        if (ctitle[0] && (strstr(ctitle, title) || strstr(title, ctitle))) {
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
                Target *t = &out[*n];
                t->x = ext->x; t->y = ext->y;
                t->w = ext->width; t->h = ext->height;
                (*n)++;
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
                /* check if this window's coords are usable */
                int zeros = 0;
                for (int t = start; t < st->n_targets; t++)
                    if (st->targets[t].x == 0 && st->targets[t].y == 0) zeros++;

                if ((double)zeros / count >= 0.8) {
                    /* broken coords — try to find this window's geometry
                     * from hyprctl and distribute in a grid */
                    int wx = 0, wy = 0, ww = 0, wh = 0;
                    gboolean found = find_client_geom(clients_json,
                                                       (int)app_pid,
                                                       &wx, &wy, &ww, &wh);
                    if (!found) {
                        /* try matching by window title */
                        gchar *wtitle = atspi_accessible_get_name(w, NULL);
                        if (wtitle) {
                            found = find_client_geom_by_title(clients_json,
                                        wtitle, &wx, &wy, &ww, &wh);
                            g_free(wtitle);
                        }
                    }
                    if (found && ww > 0 && wh > 0) {
                        int m = 30;
                        int gx = wx + m, gy = wy + m;
                        int gw = ww - m*2, gh = wh - m*2;
                        int cols = (int)ceil(sqrt((double)count));
                        int rows = (int)ceil((double)count / cols);
                        double cw = (double)gw / (cols ? cols : 1);
                        double ch = (double)gh / (rows ? rows : 1);
                        for (int t = 0; t < count; t++) {
                            st->targets[start + t].x = (int)(gx + (t % cols) * cw + cw / 2);
                            st->targets[start + t].y = (int)(gy + (t / cols) * ch + ch / 2);
                        }
                    } else {
                        /* can't find window geom — drop these targets */
                        st->n_targets = start;
                    }
                } else {
                    /* good coords — center them */
                    for (int t = start; t < st->n_targets; t++) {
                        st->targets[t].x += st->targets[t].w / 2;
                        st->targets[t].y += st->targets[t].h / 2;
                    }
                }
            }

            g_object_unref(w);
        }
        g_object_unref(app);
    }
}

/* ------------------------------------------------------------------ */
/*  ydotool — direct exec, no shell                                    */
/* ------------------------------------------------------------------ */

static void do_click(int x, int y) {
    int yx = (int)round((double)x / YDOTOOL_RATIO);
    int yy = (int)round((double)y / YDOTOOL_RATIO);

    char sx[16], sy[16];
    snprintf(sx, sizeof(sx), "%d", yx);
    snprintf(sy, sizeof(sy), "%d", yy);

    pid_t pid = fork();
    if (pid == 0) {
        setenv("YDOTOOL_SOCKET", "/tmp/.ydotool_socket", 1);
        execlp("ydotool", "ydotool", "mousemove", "-a", "-x", sx, "-y", sy, NULL);
        _exit(1);
    }
    if (pid > 0) waitpid(pid, NULL, 0);

    pid = fork();
    if (pid == 0) {
        setenv("YDOTOOL_SOCKET", "/tmp/.ydotool_socket", 1);
        execlp("ydotool", "ydotool", "click", "0xC0", NULL);
        _exit(1);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}

/* ------------------------------------------------------------------ */
/*  overlay                                                            */
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
        s->click_x = s->targets[mi].x;
        s->click_y = s->targets[mi].y;
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
        gtk_fixed_put(GTK_FIXED(fixed), lbl, s->targets[i].x, s->targets[i].y);
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
        do_click(s->click_x, s->click_y);
    }
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    init_clickable_lut();
    atspi_init();

    /* fetch all hyprland client geometries in one call */
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

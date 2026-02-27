/*
 * wlim — vimium-like click hints for wayland (hyprland)
 *
 * walks the AT-SPI2 accessibility tree of the focused window,
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

#define MAX_TARGETS  512
#define MAX_LABEL    4
#define MAX_TYPED    8

/* ydotool absolute coordinates on hyprland don't map 1:1 to screen
 * pixels. this ratio was determined experimentally — your setup may
 * differ if you change monitor config or scale. */
#define YDOTOOL_RATIO 2.375

static const AtspiRole CLICKABLE_ROLES[] = {
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
#define N_CLICKABLE (sizeof(CLICKABLE_ROLES) / sizeof(CLICKABLE_ROLES[0]))

typedef struct {
    int x, y, w, h;
    char label[MAX_LABEL + 1];
    char name[128];
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
/*  helpers                                                            */
/* ------------------------------------------------------------------ */

static gboolean is_clickable(AtspiRole role) {
    for (size_t i = 0; i < N_CLICKABLE; i++)
        if (CLICKABLE_ROLES[i] == role) return TRUE;
    return FALSE;
}

static char *run_cmd(const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    while (1) {
        size_t n = fread(buf + len, 1, cap - len - 1, fp);
        if (n == 0) break;
        len += n;
        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
    }
    buf[len] = '\0';
    pclose(fp);
    return buf;
}

/* dumb json helpers — good enough for hyprctl output */

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

    /* skip invisible stuff (but always descend into root) */
    if (depth > 0) {
        AtspiStateSet *ss = atspi_accessible_get_state_set(node);
        if (ss) {
            gboolean ok = atspi_state_set_contains(ss, ATSPI_STATE_VISIBLE)
                       && atspi_state_set_contains(ss, ATSPI_STATE_SHOWING);
            g_object_unref(ss);
            if (!ok) goto kids;
        }
    }

    if (is_clickable(role)) {
        AtspiComponent *comp = atspi_accessible_get_component_iface(node);
        if (comp) {
            AtspiRect *ext = atspi_component_get_extents(
                comp, ATSPI_COORD_TYPE_SCREEN, &err);
            if (ext && !err) {
                Target *t = &out[*n];
                t->x = ext->x; t->y = ext->y;
                t->w = ext->width; t->h = ext->height;
                gchar *name = atspi_accessible_get_name(node, NULL);
                if (name) {
                    strncpy(t->name, name, sizeof(t->name) - 1);
                    t->name[sizeof(t->name) - 1] = '\0';
                    g_free(name);
                } else {
                    t->name[0] = '\0';
                }
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

static AtspiAccessible *find_active_window(void) {
    char *j = run_cmd("hyprctl -j activewindow");
    char wclass[128] = {0};
    int wpid = 0;
    if (j) {
        json_str(j, "class", wclass, sizeof(wclass));
        wpid = json_int(j, "pid", 0);
        for (char *p = wclass; *p; p++) *p = g_ascii_tolower(*p);
        free(j);
    }

    AtspiAccessible *desktop = atspi_get_desktop(0);
    int napps = atspi_accessible_get_child_count(desktop, NULL);

    /* try ACTIVE state first */
    for (int i = 0; i < napps; i++) {
        AtspiAccessible *app = atspi_accessible_get_child_at_index(desktop, i, NULL);
        if (!app) continue;
        int nw = atspi_accessible_get_child_count(app, NULL);
        for (int k = 0; k < nw; k++) {
            AtspiAccessible *w = atspi_accessible_get_child_at_index(app, k, NULL);
            if (!w) continue;
            AtspiStateSet *ss = atspi_accessible_get_state_set(w);
            if (ss && atspi_state_set_contains(ss, ATSPI_STATE_ACTIVE)) {
                g_object_unref(ss); g_object_unref(app);
                return w;
            }
            if (ss) g_object_unref(ss);
            g_object_unref(w);
        }
        g_object_unref(app);
    }

    /* fallback: match pid or class name */
    for (int i = 0; i < napps; i++) {
        AtspiAccessible *app = atspi_accessible_get_child_at_index(desktop, i, NULL);
        if (!app) continue;
        gchar *aname = atspi_accessible_get_name(app, NULL);
        char lower[128] = {0};
        if (aname) {
            strncpy(lower, aname, sizeof(lower) - 1);
            for (char *p = lower; *p; p++) *p = g_ascii_tolower(*p);
            g_free(aname);
        }
        if (wpid > 0) {
            guint pid = atspi_accessible_get_process_id(app, NULL);
            if ((int)pid == wpid) {
                int nw = atspi_accessible_get_child_count(app, NULL);
                for (int k = 0; k < nw; k++) {
                    AtspiAccessible *w = atspi_accessible_get_child_at_index(app, k, NULL);
                    if (w) { g_object_unref(app); return w; }
                }
            }
        }
        if (wclass[0] && (strstr(lower, wclass) || strstr(wclass, lower))) {
            int nw = atspi_accessible_get_child_count(app, NULL);
            for (int k = 0; k < nw; k++) {
                AtspiAccessible *w = atspi_accessible_get_child_at_index(app, k, NULL);
                if (w) { g_object_unref(app); return w; }
            }
        }
        g_object_unref(app);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  coordinate fixup                                                   */
/* ------------------------------------------------------------------ */

static gboolean coords_ok(Target *t, int n) {
    if (n == 0) return FALSE;
    int z = 0;
    for (int i = 0; i < n; i++)
        if (t[i].x == 0 && t[i].y == 0) z++;
    return (double)z / n < 0.8;
}

static void grid_fallback(Target *t, int n) {
    int wx = 100, wy = 100, ww = 800, wh = 600;
    char *j = run_cmd("hyprctl -j activewindow");
    if (j) {
        json_int_pair(j, "at", &wx, &wy);
        json_int_pair(j, "size", &ww, &wh);
        free(j);
    }
    int m = 30;
    wx += m; wy += m; ww -= m*2; wh -= m*2;
    int cols = (int)ceil(sqrt((double)n));
    int rows = (int)ceil((double)n / cols);
    double cw = (double)ww / (cols ? cols : 1);
    double ch = (double)wh / (rows ? rows : 1);
    for (int i = 0; i < n; i++) {
        t[i].x = (int)(wx + (i % cols) * cw + cw / 2);
        t[i].y = (int)(wy + (i / cols) * ch + ch / 2);
    }
}

/* ------------------------------------------------------------------ */
/*  ydotool                                                            */
/* ------------------------------------------------------------------ */

static void do_click(int x, int y) {
    int yx = (int)round((double)x / YDOTOOL_RATIO);
    int yy = (int)round((double)y / YDOTOOL_RATIO);
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "YDOTOOL_SOCKET=/tmp/.ydotool_socket ydotool mousemove -a -x %d -y %d && "
        "YDOTOOL_SOCKET=/tmp/.ydotool_socket ydotool click 0xC0", yx, yy);
    system(cmd);
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

    /* exact match → click */
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

    /* nothing possible → reset */
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
    atspi_init();

    AtspiAccessible *win = find_active_window();
    if (!win) {
        system("notify-send -t 3000 wlim 'no accessible elements — app may not support at-spi'");
        return 1;
    }

    State st = {0};
    walk(win, st.targets, &st.n_targets, 0);
    g_object_unref(win);

    if (st.n_targets == 0) {
        system("notify-send -t 3000 wlim 'no clickable elements found'");
        return 1;
    }

    if (coords_ok(st.targets, st.n_targets)) {
        for (int i = 0; i < st.n_targets; i++) {
            st.targets[i].x += st.targets[i].w / 2;
            st.targets[i].y += st.targets[i].h / 2;
        }
    } else {
        grid_fallback(st.targets, st.n_targets);
    }

    generate_labels(st.targets, st.n_targets);

    GtkApplication *app = gtk_application_new("dev.wlim.overlay", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &st);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), &st);
    g_application_run(G_APPLICATION(app), 0, NULL);
    g_object_unref(app);
    return 0;
}

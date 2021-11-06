#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <fontconfig/fontconfig.h>

#define LOG_MODULE "config"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "debug.h"
#include "input.h"
#include "macros.h"
#include "tokenize.h"
#include "util.h"
#include "wayland.h"
#include "xmalloc.h"

static const uint32_t default_foreground = 0xdcdccc;
static const uint32_t default_background = 0x111111;

#define cube6(r, g) \
    r|g|0x00, r|g|0x5f, r|g|0x87, r|g|0xaf, r|g|0xd7, r|g|0xff

#define cube36(r) \
    cube6(r, 0x0000), \
    cube6(r, 0x5f00), \
    cube6(r, 0x8700), \
    cube6(r, 0xaf00), \
    cube6(r, 0xd700), \
    cube6(r, 0xff00)

static const uint32_t default_color_table[256] = {
    // Regular
    0x222222,
    0xcc9393,
    0x7f9f7f,
    0xd0bf8f,
    0x6ca0a3,
    0xdc8cc3,
    0x93e0e3,
    0xdcdccc,

    // Bright
    0x666666,
    0xdca3a3,
    0xbfebbf,
    0xf0dfaf,
    0x8cd0d3,
    0xfcace3,
    0xb3ffff,
    0xffffff,

    // 6x6x6 RGB cube
    // (color channels = i ? i*40+55 : 0, where i = 0..5)
    cube36(0x000000),
    cube36(0x5f0000),
    cube36(0x870000),
    cube36(0xaf0000),
    cube36(0xd70000),
    cube36(0xff0000),

    // 24 shades of gray
    // (color channels = i*10+8, where i = 0..23)
    0x080808, 0x121212, 0x1c1c1c, 0x262626,
    0x303030, 0x3a3a3a, 0x444444, 0x4e4e4e,
    0x585858, 0x626262, 0x6c6c6c, 0x767676,
    0x808080, 0x8a8a8a, 0x949494, 0x9e9e9e,
    0xa8a8a8, 0xb2b2b2, 0xbcbcbc, 0xc6c6c6,
    0xd0d0d0, 0xdadada, 0xe4e4e4, 0xeeeeee
};

static const char *const binding_action_map[] = {
    [BIND_ACTION_NONE] = NULL,
    [BIND_ACTION_NOOP] = "noop",
    [BIND_ACTION_SCROLLBACK_UP_PAGE] = "scrollback-up-page",
    [BIND_ACTION_SCROLLBACK_UP_HALF_PAGE] = "scrollback-up-half-page",
    [BIND_ACTION_SCROLLBACK_UP_LINE] = "scrollback-up-line",
    [BIND_ACTION_SCROLLBACK_DOWN_PAGE] = "scrollback-down-page",
    [BIND_ACTION_SCROLLBACK_DOWN_HALF_PAGE] = "scrollback-down-half-page",
    [BIND_ACTION_SCROLLBACK_DOWN_LINE] = "scrollback-down-line",
    [BIND_ACTION_CLIPBOARD_COPY] = "clipboard-copy",
    [BIND_ACTION_CLIPBOARD_PASTE] = "clipboard-paste",
    [BIND_ACTION_PRIMARY_PASTE] = "primary-paste",
    [BIND_ACTION_SEARCH_START] = "search-start",
    [BIND_ACTION_FONT_SIZE_UP] = "font-increase",
    [BIND_ACTION_FONT_SIZE_DOWN] = "font-decrease",
    [BIND_ACTION_FONT_SIZE_RESET] = "font-reset",
    [BIND_ACTION_SPAWN_TERMINAL] = "spawn-terminal",
    [BIND_ACTION_MINIMIZE] = "minimize",
    [BIND_ACTION_MAXIMIZE] = "maximize",
    [BIND_ACTION_FULLSCREEN] = "fullscreen",
    [BIND_ACTION_PIPE_SCROLLBACK] = "pipe-scrollback",
    [BIND_ACTION_PIPE_VIEW] = "pipe-visible",
    [BIND_ACTION_PIPE_SELECTED] = "pipe-selected",
    [BIND_ACTION_SHOW_URLS_COPY] = "show-urls-copy",
    [BIND_ACTION_SHOW_URLS_LAUNCH] = "show-urls-launch",

    /* Mouse-specific actions */
    [BIND_ACTION_SELECT_BEGIN] = "select-begin",
    [BIND_ACTION_SELECT_BEGIN_BLOCK] = "select-begin-block",
    [BIND_ACTION_SELECT_EXTEND] = "select-extend",
    [BIND_ACTION_SELECT_EXTEND_CHAR_WISE] = "select-extend-character-wise",
    [BIND_ACTION_SELECT_WORD] = "select-word",
    [BIND_ACTION_SELECT_WORD_WS] = "select-word-whitespace",
    [BIND_ACTION_SELECT_ROW] = "select-row",
};

static_assert(ALEN(binding_action_map) == BIND_ACTION_COUNT,
              "binding action map size mismatch");

struct context {
    struct config *conf;
    const char *section;
    const char *key;
    const char *value;

    const char *path;
    unsigned lineno;

    bool errors_are_fatal;
};

static const enum user_notification_kind log_class_to_notify_kind[LOG_CLASS_COUNT] = {
    [LOG_CLASS_WARNING] = USER_NOTIFICATION_WARNING,
    [LOG_CLASS_ERROR] = USER_NOTIFICATION_ERROR,
};

static void NOINLINE VPRINTF(5)
log_and_notify_va(struct config *conf, enum log_class log_class,
                  const char *file, int lineno, const char *fmt, va_list va)
{
    xassert(log_class < ALEN(log_class_to_notify_kind));
    enum user_notification_kind kind = log_class_to_notify_kind[log_class];

    if (kind == 0) {
        BUG("unsupported log class: %d", (int)log_class);
        return;
    }

    char *formatted_msg = xvasprintf(fmt, va);
    log_msg(log_class, LOG_MODULE, file, lineno, formatted_msg);
    tll_push_back(
        conf->notifications,
        ((struct user_notification){.kind = kind, .text = formatted_msg}));
}

static void NOINLINE PRINTF(5)
log_and_notify(struct config *conf, enum log_class log_class,
               const char *file, int lineno, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    log_and_notify_va(conf, log_class, file, lineno, fmt, va);
    va_end(va);
}

static void NOINLINE PRINTF(5)
log_contextual(struct context *ctx, enum log_class log_class,
               const char *file, int lineno, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    char *formatted_msg = xvasprintf(fmt, va);
    va_end(va);

    log_and_notify(
        ctx->conf, log_class, file, lineno, "%s:%d: [%s].%s: %s: %s",
        ctx->path, ctx->lineno, ctx->section, ctx->key, ctx->value,
        formatted_msg);
    free(formatted_msg);
}


static void NOINLINE VPRINTF(4)
log_and_notify_errno_va(struct config *conf, const char *file, int lineno,
                     const char *fmt, va_list va)
{
    int errno_copy = errno;
    char *formatted_msg = xvasprintf(fmt, va);
    log_and_notify(
        conf, LOG_CLASS_ERROR, file, lineno,
        "%s: %s", formatted_msg, strerror(errno_copy));
    free(formatted_msg);
}

static void NOINLINE PRINTF(4)
log_and_notify_errno(struct config *conf, const char *file, int lineno,
                     const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    log_and_notify_errno_va(conf, file, lineno, fmt, va);
    va_end(va);
}

static void NOINLINE PRINTF(4)
log_contextual_errno(struct context *ctx, const char *file, int lineno,
                     const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    char *formatted_msg = xvasprintf(fmt, va);
    va_end(va);

    log_and_notify_errno(
        ctx->conf, file, lineno, "%s:%d: [%s].%s: %s: %s",
        ctx->path, ctx->lineno, ctx->section, ctx->key, ctx->value,
        formatted_msg);

    free(formatted_msg);
}

#define LOG_CONTEXTUAL_ERR(...) \
    log_contextual(ctx, LOG_CLASS_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_CONTEXTUAL_WARN(...) \
    log_contextual(ctx, LOG_CLASS_WARNING, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_CONTEXTUAL_ERRNO(...) \
    log_contextual_errno(ctx, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_AND_NOTIFY_ERR(...) \
    log_and_notify(conf, LOG_CLASS_ERROR, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_AND_NOTIFY_WARN(...) \
    log_and_notify(conf, LOG_CLASS_WARNING, __FILE__, __LINE__, __VA_ARGS__)

#define LOG_AND_NOTIFY_ERRNO(...) \
    log_and_notify_errno(conf, __FILE__, __LINE__, __VA_ARGS__)

static char *
get_shell(void)
{
    const char *shell = getenv("SHELL");

    if (shell == NULL) {
        struct passwd *passwd = getpwuid(getuid());
        if (passwd == NULL) {
            LOG_ERRNO("failed to lookup user: falling back to 'sh'");
            shell = "sh";
        } else
            shell = passwd->pw_shell;
    }

    LOG_DBG("user's shell: %s", shell);
    return xstrdup(shell);
}

struct config_file {
    char *path;       /* Full, absolute, path */
    int fd;           /* FD of file, O_RDONLY */
};

struct path_component {
    const char *component;
    int fd;
};
typedef tll(struct path_component) path_components_t;

static void NOINLINE
path_component_add(path_components_t *components, const char *comp, int fd)
{
    xassert(comp != NULL);
    xassert(fd >= 0);

    struct path_component pc = {.component = comp, .fd = fd};
    tll_push_back(*components, pc);
}

static void NOINLINE
path_component_destroy(struct path_component *component)
{
    xassert(component->fd >= 0);
    close(component->fd);
}

static void NOINLINE
path_components_destroy(path_components_t *components)
{
    tll_foreach(*components, it) {
        path_component_destroy(&it->item);
        tll_remove(*components, it);
    }
}

static struct config_file
path_components_to_config_file(const path_components_t *components)
{
    if (tll_length(*components) == 0)
        goto err;

    size_t len = 0;
    tll_foreach(*components, it)
        len += strlen(it->item.component) + 1;

    char *path = malloc(len);
    if (path == NULL)
        goto err;

    size_t idx = 0;
    tll_foreach(*components, it) {
        strcpy(&path[idx], it->item.component);
        idx += strlen(it->item.component);
        path[idx++] = '/';
    }
    path[idx - 1] = '\0';  /* Strip last ’/’ */

    int fd_copy = dup(tll_back(*components).fd);
    if (fd_copy < 0) {
        free(path);
        goto err;
    }

    return (struct config_file){.path = path, .fd = fd_copy};

err:
    return (struct config_file){.path = NULL, .fd = -1};
}

static const char *
get_user_home_dir(void)
{
    const struct passwd *passwd = getpwuid(getuid());
    if (passwd == NULL)
        return NULL;
    return passwd->pw_dir;
}

static bool
try_open_file(path_components_t *components, const char *name)
{
    int parent_fd = tll_back(*components).fd;

    struct stat st;
    if (fstatat(parent_fd, name, &st, 0) == 0 && S_ISREG(st.st_mode)) {
        int fd = openat(parent_fd, name, O_RDONLY);
        if (fd >= 0) {
            path_component_add(components, name, fd);
            return true;
        }
    }

    return false;
}

static struct config_file
open_config(void)
{
    struct config_file ret = {.path = NULL, .fd = -1};

    path_components_t components = tll_init();

    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    const char *user_home_dir = get_user_home_dir();
    char *xdg_config_dirs_copy = NULL;

    /* Use XDG_CONFIG_HOME, or ~/.config */
    if (xdg_config_home != NULL) {
        int fd = open(xdg_config_home, O_RDONLY);
        if (fd >= 0)
            path_component_add(&components, xdg_config_home, fd);
    } else if (user_home_dir != NULL) {
        int home_fd = open(user_home_dir, O_RDONLY);
        if (home_fd >= 0) {
            int config_fd = openat(home_fd, ".config", O_RDONLY);
            if (config_fd >= 0) {
                path_component_add(&components, user_home_dir, home_fd);
                path_component_add(&components, ".config", config_fd);
            } else
                close(home_fd);
        }
    }

    /* First look for foot/foot.ini */
    if (tll_length(components) > 0) {
        int foot_fd = openat(tll_back(components).fd, "foot", O_RDONLY);
        if (foot_fd >= 0) {
            path_component_add(&components, "foot", foot_fd);

            if (try_open_file(&components, "foot.ini"))
                goto done;

            struct path_component pc = tll_pop_back(components);
            path_component_destroy(&pc);
        }
    }

    /* Finally, try foot/foot.ini in all XDG_CONFIG_DIRS */
    const char *xdg_config_dirs = getenv("XDG_CONFIG_DIRS");
    xdg_config_dirs_copy = xdg_config_dirs != NULL
        ? strdup(xdg_config_dirs) : NULL;

    if (xdg_config_dirs_copy != NULL) {
        for (char *save = NULL,
                 *xdg_dir = strtok_r(xdg_config_dirs_copy, ":", &save);
             xdg_dir != NULL;
             xdg_dir = strtok_r(NULL, ":", &save))
        {
            path_components_destroy(&components);

            int xdg_fd = open(xdg_dir, O_RDONLY);
            if (xdg_fd < 0)
                continue;

            int foot_fd = openat(xdg_fd, "foot", O_RDONLY);
            if (foot_fd < 0) {
                close(xdg_fd);
                continue;
            }

            xassert(tll_length(components) == 0);
            path_component_add(&components, xdg_dir, xdg_fd);
            path_component_add(&components, "foot", foot_fd);

            if (try_open_file(&components, "foot.ini"))
                goto done;
        }
    }

out:
    path_components_destroy(&components);
    free(xdg_config_dirs_copy);
    return ret;

done:
    xassert(tll_length(components) > 0);
    ret = path_components_to_config_file(&components);
    goto out;
}

static int
wccmp(const void *_a, const void *_b)
{
    const wchar_t *a = _a;
    const wchar_t *b = _b;
    return *a - *b;
}

static bool
str_has_prefix(const char *str, const char *prefix)
{
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

static bool NOINLINE
value_to_bool(struct context *ctx, bool *res)
{
    static const char *const yes[] = {"on", "true", "yes", "1"};
    static const char *const  no[] = {"off", "false", "no", "0"};

    for (size_t i = 0; i < ALEN(yes); i++) {
        if (strcasecmp(ctx->value, yes[i]) == 0) {
            *res = true;
            return true;
        }
    }

    for (size_t i = 0; i < ALEN(no); i++) {
        if (strcasecmp(ctx->value, no[i]) == 0) {
            *res = false;
            return true;
        }
    }

    LOG_CONTEXTUAL_ERR("invalid boolean value");
    return false;
}


static bool NOINLINE
str_to_ulong(const char *s, int base, unsigned long *res)
{
    if (s == NULL)
        return false;

    errno = 0;
    char *end = NULL;

    *res = strtoul(s, &end, base);
    return errno == 0 && *end == '\0';
}

static bool NOINLINE
str_to_uint32(const char *s, int base, uint32_t *res)
{
    unsigned long v;
    bool ret = str_to_ulong(s, base, &v);
    if (v > UINT32_MAX)
        return false;
    *res = v;
    return ret;
}

static bool NOINLINE
str_to_uint16(const char *s, int base, uint16_t *res)
{
    unsigned long v;
    bool ret = str_to_ulong(s, base, &v);
    if (v > UINT16_MAX)
        return false;
    *res = v;
    return ret;
}

static bool NOINLINE
value_to_uint16(struct context *ctx, int base, uint16_t *res)
{
    if (!str_to_uint16(ctx->value, base, res)) {
        LOG_CONTEXTUAL_ERR(
            "invalid integer value, or outside range 0-%u", UINT16_MAX);
        return false;
    }
    return true;
}

static bool NOINLINE
value_to_uint32(struct context *ctx, int base, uint32_t *res)
{
    if (!str_to_uint32(ctx->value, base, res)){
        LOG_CONTEXTUAL_ERR(
            "invalid integer value, or outside range 0-%u", UINT32_MAX);
        return false;
    }
    return true;
}

static bool NOINLINE
value_to_dimensions(struct context *ctx, uint32_t *x, uint32_t *y)
{
    if (sscanf(ctx->value, "%ux%u", x, y) != 2) {
        LOG_CONTEXTUAL_ERR("invalid dimensions (must be on the form AxB)");
        return false;
    }

    return true;
}

static bool NOINLINE
value_to_double(struct context *ctx, float *res)
{
    const char *s = ctx->value;

    if (s == NULL)
        return false;

    errno = 0;
    char *end = NULL;

    *res = strtof(s, &end);
    if (!(errno == 0 && *end == '\0')) {
        LOG_CONTEXTUAL_ERR("invalid decimal value");
        return false;
    }

    return true;
}

static bool NOINLINE
value_to_str(struct context *ctx, char **res)
{
    free(*res);
    *res = xstrdup(ctx->value);
    return true;
}

static bool NOINLINE
value_to_wchars(struct context *ctx, wchar_t **res)
{
    *res = NULL;

    size_t chars = mbstowcs(NULL, ctx->value, 0);
    if (chars == (size_t)-1) {
        LOG_CONTEXTUAL_ERR("not a valid string value");
        return false;
    }

    *res = xmalloc((chars + 1) * sizeof(wchar_t));
    mbstowcs(*res, ctx->value, chars + 1);
    return true;
}

static int NOINLINE
value_to_enum(struct context *ctx, const char **value_map)
{
    size_t str_len = 0;
    size_t count = 0;

    for (; value_map[count] != NULL; count++) {
        if (strcasecmp(value_map[count], ctx->value) == 0)
            return count;
        str_len += strlen(value_map[count]);
    }

    const size_t size = str_len + count * 4 + 1;
    char *valid_values = xmalloc(size);
    int idx = 0;

    for (size_t i = 0; i < count; i++)
        idx += snprintf(&valid_values[idx], size - idx, "'%s', ", value_map[i]);

    if (count > 0)
        valid_values[idx - 2] = '\0';

    LOG_CONTEXTUAL_ERR("not one of %s", valid_values);
    return -1;
}

static bool NOINLINE
value_to_color(struct context *ctx, uint32_t *color, bool allow_alpha)
{
    if (!str_to_uint32(ctx->value, 16, color)) {
        LOG_CONTEXTUAL_ERR("not a valid color value");
        return false;
    }

    if (!allow_alpha && (*color & 0xff000000) != 0) {
        LOG_CONTEXTUAL_ERR("color value must not have an alpha component");
        return false;
    }

    return true;
}

static bool NOINLINE
value_to_two_colors(struct context *ctx,
                    uint32_t *first, uint32_t *second, bool allow_alpha)
{
    bool ret = false;
    const char *original_value = ctx->value;

    /* TODO: do this without strdup() */
    char *value_copy = xstrdup(ctx->value);
    const char *first_as_str = strtok(value_copy, " ");
    const char *second_as_str = strtok(NULL, " ");

    if (first_as_str == NULL || second_as_str == NULL) {
        LOG_CONTEXTUAL_ERR("invalid double color value");
        goto out;
    }

    ctx->value = first_as_str;
    if (!value_to_color(ctx, first, allow_alpha))
        goto out;

    ctx->value = second_as_str;
    if (!value_to_color(ctx, second, allow_alpha))
        goto out;

    ret = true;

out:
    free(value_copy);
    ctx->value = original_value;
    return ret;
}

static bool NOINLINE
value_to_pt_or_px(struct context *ctx, struct pt_or_px *res)
{
    const char *s = ctx->value;

    size_t len = s != NULL ? strlen(s) : 0;
    if (len >= 2 && s[len - 2] == 'p' && s[len - 1] == 'x') {
        errno = 0;
        char *end = NULL;

        long value = strtol(s, &end, 10);
        if (!(errno == 0 && end == s + len - 2)) {
            LOG_CONTEXTUAL_ERR("invalid px value (must be on the form 12px)");
            return false;
        }
        res->pt = 0;
        res->px = value;
    } else {
        float value;
        if (!value_to_double(ctx, &value))
            return false;
        res->pt = value;
        res->px = 0;
    }

    return true;
}

static struct config_font_list NOINLINE
value_to_fonts(struct context *ctx)
{
    size_t count = 0;
    size_t size = 0;
    struct config_font *fonts = NULL;

    char *copy = xstrdup(ctx->value);
    for (const char *font = strtok(copy, ",");
         font != NULL;
         font = strtok(NULL, ","))
    {
        /* Trim spaces, strictly speaking not necessary, but looks nice :) */
        while (*font != '\0' && isspace(*font))
            font++;

        if (font[0] == '\0')
            continue;

        struct config_font font_data;
        if (!config_font_parse(font, &font_data)) {
            ctx->value = font;
            LOG_CONTEXTUAL_ERR("invalid font specification");
            goto err;
        }

        if (count + 1 > size) {
            size += 4;
            fonts = xrealloc(fonts, size * sizeof(fonts[0]));
        }

        xassert(count + 1 <= size);
        fonts[count++] = font_data;
    }

    free(copy);
    return (struct config_font_list){.arr = fonts, .count = count};

err:
    free(copy);
    free(fonts);
    return (struct config_font_list){.arr = NULL, .count = 0};
}

static void NOINLINE
free_argv(struct argv *argv)
{
    if (argv->args == NULL)
        return;
    for (char **a = argv->args; *a != NULL; a++)
        free(*a);
    free(argv->args);
    argv->args = NULL;
}

static void NOINLINE
clone_argv(struct argv *dst, const struct argv *src)
{
    if (src->args == NULL) {
        dst->args = NULL;
        return;
    }

    size_t count = 0;
    for (char **args = src->args; *args != NULL; args++)
        count++;

    dst->args = xmalloc((count + 1) * sizeof(dst->args[0]));
    for (char **args_src = src->args, **args_dst = dst->args;
         *args_src != NULL; args_src++,
             args_dst++)
    {
        *args_dst = xstrdup(*args_src);
    }
    dst->args[count] = NULL;
}

static void
spawn_template_free(struct config_spawn_template *template)
{
    free_argv(&template->argv);
}

static void
spawn_template_clone(struct config_spawn_template *dst,
                     const struct config_spawn_template *src)
{
    clone_argv(&dst->argv, &src->argv);
}

static bool NOINLINE
value_to_spawn_template(struct context *ctx,
                        struct config_spawn_template *template)
{
    spawn_template_free(template);

    char **argv = NULL;

    if (!tokenize_cmdline(ctx->value, &argv)) {
        LOG_CONTEXTUAL_ERR("syntax error in command line");
        return false;
    }

    template->argv.args = argv;
    return true;
}

static void
deprecated_url_option(struct config *conf,
                      const char *old_name, const char *new_name,
                      const char *path, unsigned lineno)
{
    LOG_WARN(
        "deprecated: %s:%d: [main].%s: use '%s' in section '[url]' instead",
        path, lineno, old_name, new_name);

    const char fmt[] =
        "%s:%d: \033[1m%s\033[22m, use \033[1m%s\033[22m in the \033[1m[url]\033[22m section instead";
    char *text = xasprintf(fmt, path, lineno, old_name, new_name);

    struct user_notification deprecation = {
        .kind = USER_NOTIFICATION_DEPRECATED,
        .text = text,
    };
    tll_push_back(conf->notifications, deprecation);
}

static bool parse_config_file(
    FILE *f, struct config *conf, const char *path, bool errors_are_fatal);

static bool
parse_section_main(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;
    const char *value = ctx->value;
    const char *path = ctx->path;
    unsigned lineno = ctx->lineno;
    bool errors_are_fatal = ctx->errors_are_fatal;

    if (strcmp(key, "include") == 0) {
        char *_include_path = NULL;
        const char *include_path = NULL;

        if (value[0] == '~' && value[1] == '/') {
            const char *home_dir = get_user_home_dir();

            if (home_dir == NULL) {
                LOG_CONTEXTUAL_ERRNO("failed to expand '~'");
                return false;
            }

            _include_path = xasprintf("%s/%s", home_dir, value + 2);
            include_path = _include_path;
        } else
            include_path = value;

        if (include_path[0] != '/') {
            LOG_CONTEXTUAL_ERR("not an absolute path");
            free(_include_path);
            return false;
        }

        FILE *include = fopen(include_path, "r");

        if (include == NULL) {
            LOG_CONTEXTUAL_ERRNO("failed to open");
            free(_include_path);
            return false;
        }

        bool ret = parse_config_file(
            include, conf, include_path, errors_are_fatal);
        fclose(include);

        LOG_INFO("imported sub-configuration from %s", include_path);
        free(_include_path);
        return ret;
    }

    else if (strcmp(key, "term") == 0)
        return value_to_str(ctx, &conf->term);

    else if (strcmp(key, "shell") == 0)
        return value_to_str(ctx, &conf->shell);

    else if (strcmp(key, "login-shell") == 0)
        return value_to_bool(ctx, &conf->login_shell);

    else if (strcmp(key, "title") == 0)
        return value_to_str(ctx, &conf->title);

    else if (strcmp(key, "locked-title") == 0)
        return value_to_bool(ctx, &conf->locked_title);

    else if (strcmp(key, "app-id") == 0)
        return value_to_str(ctx, &conf->app_id);

    else if (strcmp(key, "initial-window-size-pixels") == 0) {
        if (!value_to_dimensions(ctx, &conf->size.width, &conf->size.height))
            return false;

        conf->size.type = CONF_SIZE_PX;
    }

    else if (strcmp(key, "initial-window-size-chars") == 0) {
        if (!value_to_dimensions(ctx, &conf->size.width, &conf->size.height))
            return false;

        conf->size.type = CONF_SIZE_CELLS;
    }

    else if (strcmp(key, "pad") == 0) {
        unsigned x, y;
        char mode[16] = {0};

        int ret = sscanf(value, "%ux%u %15s", &x, &y, mode);
        bool center = strcasecmp(mode, "center") == 0;
        bool invalid_mode = !center && mode[0] != '\0';

        if ((ret != 2 && ret != 3) || invalid_mode) {
            LOG_CONTEXTUAL_ERR(
                "invalid padding (must be on the form PAD_XxPAD_Y [center])");
            return false;
        }

        conf->pad_x = x;
        conf->pad_y = y;
        conf->center = center;
    }

    else if (strcmp(key, "resize-delay-ms") == 0)
        return value_to_uint16(ctx, 10, &conf->resize_delay_ms);

    else if (strcmp(key, "bold-text-in-bright") == 0) {
        if (strcmp(value, "palette-based") == 0) {
            conf->bold_in_bright.enabled = true;
            conf->bold_in_bright.palette_based = true;
        } else {
            if (!value_to_bool(ctx, &conf->bold_in_bright.enabled))
                return false;
            conf->bold_in_bright.palette_based = false;
        }
    }

    else if (strcmp(key, "bell") == 0) {
        LOG_WARN(
            "deprecated: %s:%d: [main].bell: "
            "set actions in section '[bell]' instead", path, lineno);

        const char fmt[] =
            "%s:%d: \033[1mbell\033[22m, use \033[1murgent\033[22m in "
            "the \033[1m[bell]\033[22m section instead";

        struct user_notification deprecation = {
            .kind = USER_NOTIFICATION_DEPRECATED,
            .text = xasprintf(fmt, path, lineno),
        };
        tll_push_back(conf->notifications, deprecation);

        int bell = value_to_enum(
            ctx, (const char *[]){"set-urgency", "notify", "none", NULL});

        switch (bell) {
        case 0: conf->bell.urgent = true; break;
        case 1: conf->bell.notify = true; break;
        case 2: memset(&conf->bell, 0, sizeof(conf->bell)); break;

        case -1:
        default:
            return false;

        }
    }

    else if (strcmp(key, "initial-window-mode") == 0) {
        int mode = value_to_enum(
            ctx, (const char *[]){"windowed", "maximized", "fullscreen", NULL});

        if (mode < 0)
            return false;

        conf->startup_mode = mode;
    }

    else if (strcmp(key, "font") == 0 ||
             strcmp(key, "font-bold") == 0 ||
             strcmp(key, "font-italic") == 0 ||
             strcmp(key, "font-bold-italic") == 0)

    {
        size_t idx =
            strcmp(key, "font") == 0 ? 0 :
            strcmp(key, "font-bold") == 0 ? 1 :
            strcmp(key, "font-italic") == 0 ? 2 : 3;

        struct config_font_list new_list = value_to_fonts(ctx);
        if (new_list.arr == NULL)
            return false;

        config_font_list_destroy(&conf->fonts[idx]);
        conf->fonts[idx] = new_list;
    }

    else if (strcmp(key, "line-height") == 0) {
        if (!value_to_pt_or_px(ctx, &conf->line_height))
            return false;
    }

    else if (strcmp(key, "letter-spacing") == 0) {
        if (!value_to_pt_or_px(ctx, &conf->letter_spacing))
            return false;
    }

    else if (strcmp(key, "horizontal-letter-offset") == 0) {
        if (!value_to_pt_or_px(ctx, &conf->horizontal_letter_offset))
            return false;
    }

    else if (strcmp(key, "vertical-letter-offset") == 0) {
        if (!value_to_pt_or_px(ctx, &conf->vertical_letter_offset))
            return false;
    }

    else if (strcmp(key, "underline-offset") == 0) {
        if (!value_to_pt_or_px(ctx, &conf->underline_offset))
            return false;
        conf->use_custom_underline_offset = true;
    }

    else if (strcmp(key, "dpi-aware") == 0) {
        if (strcmp(value, "auto") == 0)
            conf->dpi_aware = DPI_AWARE_AUTO;
        else {
            bool value;
            if (!value_to_bool(ctx, &value))
                return false;
            conf->dpi_aware = value ? DPI_AWARE_YES : DPI_AWARE_NO;
        }
    }

    else if (strcmp(key, "workers") == 0)
        return value_to_uint16(ctx, 10, &conf->render_worker_count);

    else if (strcmp(key, "word-delimiters") == 0) {
        wchar_t *word_delimiters;
        if (!value_to_wchars(ctx, &word_delimiters))
            return false;

        free(conf->word_delimiters);
        conf->word_delimiters = word_delimiters;
    }

    else if (strcmp(key, "jump-label-letters") == 0) {
        deprecated_url_option(
            conf, "jump-label-letters", "label-letters", path, lineno);

        wchar_t *letters;
        if (!value_to_wchars(ctx, &letters))
            return false;

        free(conf->url.label_letters);
        conf->url.label_letters = letters;
    }

    else if (strcmp(key, "notify") == 0) {
        if (!value_to_spawn_template(ctx, &conf->notify))
            return false;
    }

    else if (strcmp(key, "notify-focus-inhibit") == 0) {
        return value_to_bool(ctx, &conf->notify_focus_inhibit);
    }

    else if (strcmp(key, "url-launch") == 0) {
        deprecated_url_option(
            conf, "url-launch", "launch", path, lineno);

        if (!value_to_spawn_template(ctx, &conf->url.launch))
            return false;
    }

    else if (strcmp(key, "selection-target") == 0) {
        int target = value_to_enum(
            ctx, (const char *[]){"none", "primary", "clipboard", "both", NULL});

        if (target < 0)
            return false;

        conf->selection_target = target;
    }

    else if (strcmp(key, "osc8-underline") == 0) {
        deprecated_url_option(
            conf, "osc8-underline", "osc8-underline", path, lineno);

        int mode = value_to_enum(
            ctx, (const char *[]){"url-mode", "always", NULL});

        if (mode < 0)
            return false;

        conf->url.osc8_underline = mode;
    }

    else if (strcmp(key, "box-drawings-uses-font-glyphs") == 0)
        return value_to_bool(ctx, &conf->box_drawings_uses_font_glyphs);

    else {
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);
        return false;
    }

    return true;
}

static bool
parse_section_bell(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;

    if (strcmp(key, "urgent") == 0)
        return value_to_bool(ctx, &conf->bell.urgent);
    else if (strcmp(key, "notify") == 0)
        return value_to_bool(ctx, &conf->bell.notify);
    else if (strcmp(key, "command") == 0) {
        if (!value_to_spawn_template(ctx, &conf->bell.command))
            return false;
    } else if (strcmp(key, "command-focused") == 0)
        return value_to_bool(ctx, &conf->bell.command_focused);
    else {
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);
        return false;
    }

    return true;
}

static bool
parse_section_scrollback(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;
    const char *value = ctx->value;

    if (strcmp(key, "lines") == 0)
        value_to_uint32(ctx, 10, &conf->scrollback.lines);

    else if (strcmp(key, "indicator-position") == 0) {
        int position = value_to_enum(
            ctx, (const char *[]){"none", "fixed", "relative", NULL});

        if (position < 0)
            return false;

        conf->scrollback.indicator.position = position;
    }

    else if (strcmp(key, "indicator-format") == 0) {
        if (strcmp(value, "percentage") == 0) {
            conf->scrollback.indicator.format
                = SCROLLBACK_INDICATOR_FORMAT_PERCENTAGE;
        } else if (strcmp(value, "line") == 0) {
            conf->scrollback.indicator.format
                = SCROLLBACK_INDICATOR_FORMAT_LINENO;
        } else {
            free(conf->scrollback.indicator.text);
            conf->scrollback.indicator.text = NULL;

            size_t len = mbstowcs(NULL, value, 0);
            if (len == (size_t)-1) {
                LOG_CONTEXTUAL_ERRNO("invalid free form text");
                return false;
            }

            conf->scrollback.indicator.text = xcalloc(len + 1, sizeof(wchar_t));
            mbstowcs(conf->scrollback.indicator.text, value, len + 1);
        }
    }

    else if (strcmp(key, "multiplier") == 0)
        return value_to_double(ctx, &conf->scrollback.multiplier);

    else {
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);
        return false;
    }

    return true;
}

static bool
parse_section_url(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;
    const char *value = ctx->value;

    if (strcmp(key, "launch") == 0) {
        if (!value_to_spawn_template(ctx, &conf->url.launch))
            return false;
    }

    else if (strcmp(key, "label-letters") == 0) {
        wchar_t *letters;
        if (!value_to_wchars(ctx, &letters))
            return false;

        free(conf->url.label_letters);
        conf->url.label_letters = letters;
    }

    else if (strcmp(key, "osc8-underline") == 0) {
        int mode = value_to_enum(
            ctx, (const char *[]){"url-mode", "always", NULL});

        if (mode < 0)
            return false;

        conf->url.osc8_underline = mode;
    }

    else if (strcmp(key, "protocols") == 0) {
        for (size_t i = 0; i < conf->url.prot_count; i++)
            free(conf->url.protocols[i]);
        free(conf->url.protocols);

        conf->url.max_prot_len = 0;
        conf->url.prot_count = 0;
        conf->url.protocols = NULL;

        char *copy = xstrdup(value);

        for (char *prot = strtok(copy, ",");
             prot != NULL;
             prot = strtok(NULL, ","))
        {

            /* Strip leading whitespace */
            while (isspace(*prot))
                prot++;

            /* Strip trailing whitespace */
            size_t len = strlen(prot);
            while (len > 0 && isspace(prot[len - 1]))
                prot[--len] = '\0';

            size_t chars = mbstowcs(NULL, prot, 0);
            if (chars == (size_t)-1) {
                ctx->value = prot;
                LOG_CONTEXTUAL_ERRNO("invalid protocol");
                return false;
            }

            conf->url.prot_count++;
            conf->url.protocols = xrealloc(
                conf->url.protocols,
                conf->url.prot_count * sizeof(conf->url.protocols[0]));

            size_t idx = conf->url.prot_count - 1;
            conf->url.protocols[idx] = xmalloc((chars + 1 + 3) * sizeof(wchar_t));
            mbstowcs(conf->url.protocols[idx], prot, chars + 1);
            wcscpy(&conf->url.protocols[idx][chars], L"://");

            chars += 3;  /* Include the "://" */
            if (chars > conf->url.max_prot_len)
                conf->url.max_prot_len = chars;
        }

        free(copy);
    }

    else if (strcmp(key, "uri-characters") == 0) {
        wchar_t *uri_characters;
        if (!value_to_wchars(ctx, &uri_characters))
            return false;

        free(conf->url.uri_characters);

        qsort(
            uri_characters,
            wcslen(uri_characters),
            sizeof(uri_characters[0]),
            &wccmp);
        conf->url.uri_characters = uri_characters;
    }

    else {
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);
        return false;
    }

    return true;
}

static bool
parse_section_colors(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;

    size_t key_len = strlen(key);
    uint8_t last_digit = (unsigned char)key[key_len - 1] - '0';
    uint32_t *color = NULL;

    if (isdigit(key[0])) {
        unsigned long index;
        if (!str_to_ulong(key, 0, &index) ||
            index >= ALEN(conf->colors.table))
        {
            LOG_CONTEXTUAL_ERR(
                "invalid color palette index: %s (not in range 0-%zu)",
                key, ALEN(conf->colors.table));
            return false;
        }
        color = &conf->colors.table[index];
    }

    else if (key_len == 8 && str_has_prefix(key, "regular") && last_digit < 8)
        color = &conf->colors.table[last_digit];

    else if (key_len == 7 && str_has_prefix(key, "bright") && last_digit < 8)
        color = &conf->colors.table[8 + last_digit];

    else if (strcmp(key, "foreground") == 0) color = &conf->colors.fg;
    else if (strcmp(key, "background") == 0) color = &conf->colors.bg;
    else if (strcmp(key, "selection-foreground") == 0) color = &conf->colors.selection_fg;
    else if (strcmp(key, "selection-background") == 0) color = &conf->colors.selection_bg;

    else if (strcmp(key, "jump-labels") == 0) {
        if (!value_to_two_colors(
                ctx,
                &conf->colors.jump_label.fg,
                &conf->colors.jump_label.bg,
                false))
        {
            return false;
        }

        conf->colors.use_custom.jump_label = true;
        return true;
    }

    else if (strcmp(key, "scrollback-indicator") == 0) {
        if (!value_to_two_colors(
                ctx,
                &conf->colors.scrollback_indicator.fg,
                &conf->colors.scrollback_indicator.bg,
                false))
        {
            return false;
        }

        conf->colors.use_custom.scrollback_indicator = true;
        return true;
    }

    else if (strcmp(key, "urls") == 0) {
        if (!value_to_color(ctx, &conf->colors.url, false))
            return false;

        conf->colors.use_custom.url = true;
        return true;
    }

    else if (strcmp(key, "alpha") == 0) {
        float alpha;
        if (!value_to_double(ctx, &alpha))
            return false;

        if (alpha < 0. || alpha > 1.) {
            LOG_CONTEXTUAL_ERR("not in range 0.0-1.0");
            return false;
        }

        conf->colors.alpha = alpha * 65535.;
        return true;
    }

    else {
        LOG_CONTEXTUAL_ERR("not valid option");
        return false;
    }

    uint32_t color_value;
    if (!value_to_color(ctx, &color_value, false))
        return false;

    *color = color_value;
    return true;
}

static bool
parse_section_cursor(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;

    if (strcmp(key, "style") == 0) {
        enum cursor_style style = value_to_enum(
            ctx, (const char *[]){"block", "underline", "beam", NULL});

        if (style < 0)
            return false;

        conf->cursor.style = style;
    }

    else if (strcmp(key, "blink") == 0)
        return value_to_bool(ctx, &conf->cursor.blink);

    else if (strcmp(key, "color") == 0) {
        if (!value_to_two_colors(
                ctx,
                &conf->cursor.color.text,
                &conf->cursor.color.cursor,
                false))
        {
            return false;
        }

        conf->cursor.color.text |= 1u << 31;
        conf->cursor.color.cursor |= 1u << 31;
    }

    else if (strcmp(key, "beam-thickness") == 0) {
        if (!value_to_pt_or_px(ctx, &conf->cursor.beam_thickness))
            return false;
    }

    else if (strcmp(key, "underline-thickness") == 0) {
        if (!value_to_pt_or_px(ctx, &conf->cursor.underline_thickness))
            return false;
    }

    else {
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);
        return false;
    }

    return true;
}

static bool
parse_section_mouse(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;

    if (strcmp(key, "hide-when-typing") == 0)
        return value_to_bool(ctx, &conf->mouse.hide_when_typing);

    else if (strcmp(key, "alternate-scroll-mode") == 0)
        return value_to_bool(ctx, &conf->mouse.alternate_scroll_mode);

    else {
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);
        return false;
    }

    return true;
}

static bool
parse_section_csd(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;

    if (strcmp(key, "preferred") == 0) {
        int csd = value_to_enum(
            ctx, (const char *[]){"none", "server", "client", NULL});

        if (csd < 0)
            return false;

        conf->csd.preferred = csd;
    }

    else if (strcmp(key, "font") == 0) {
        struct config_font_list new_list = value_to_fonts(ctx);
        if (new_list.arr == NULL)
            return false;

        config_font_list_destroy(&conf->csd.font);
        conf->csd.font = new_list;
    }

    else if (strcmp(key, "color") == 0) {
        uint32_t color;
        if (!value_to_color(ctx, &color, true))
            return false;

        conf->csd.color.title_set = true;
        conf->csd.color.title = color;
    }

    else if (strcmp(key, "size") == 0)
        return value_to_uint16(ctx, 10, &conf->csd.title_height);

    else if (strcmp(key, "button-width") == 0)
        return value_to_uint16(ctx, 10, &conf->csd.button_width);

    else if (strcmp(key, "button-color") == 0) {
        if (!value_to_color(ctx, &conf->csd.color.buttons, true))
            return false;

        conf->csd.color.buttons_set = true;
    }

    else if (strcmp(key, "button-minimize-color") == 0) {
        if (!value_to_color(ctx, &conf->csd.color.minimize, true))
            return false;

        conf->csd.color.minimize_set = true;
    }

    else if (strcmp(key, "button-maximize-color") == 0) {
        if (!value_to_color(ctx, &conf->csd.color.maximize, true))
            return false;

        conf->csd.color.maximize_set = true;
    }

    else if (strcmp(key, "button-close-color") == 0) {
        if (!value_to_color(ctx, &conf->csd.color.close, true))
            return false;

        conf->csd.color.close_set = true;
    }

    else if (strcmp(key, "border-color") == 0) {
        if (!value_to_color(ctx, &conf->csd.color.border, true))
            return false;

        conf->csd.color.border_set = true;
    }

    else if (strcmp(key, "border-width") == 0)
        return value_to_uint16(ctx, 10, &conf->csd.border_width_visible);

    else {
        LOG_CONTEXTUAL_ERR("not a valid action: %s", key);
        return false;
    }

    return true;
}

/* Struct that holds temporary key/mouse binding parsed data */
struct key_combo {
    char *text;          /* Raw text, e.g. "Control+Shift+V" */
    struct config_key_modifiers modifiers;
    union {
        xkb_keysym_t sym;    /* Key converted to an XKB symbol, e.g. XKB_KEY_V */
        struct {
            int button;
            int count;
        } m;
    };
};

struct key_combo_list {
    size_t count;
    struct key_combo *combos;
};

static void NOINLINE
free_key_combo_list(struct key_combo_list *key_combos)
{
    for (size_t i = 0; i < key_combos->count; i++)
        free(key_combos->combos[i].text);
    free(key_combos->combos);
    key_combos->count = 0;
    key_combos->combos = NULL;
}

static bool
parse_modifiers(struct context *ctx, const char *text, size_t len,
                struct config_key_modifiers *modifiers)
{
    bool ret = false;

    *modifiers = (struct config_key_modifiers){0};
    char *copy = xstrndup(text, len);

    for (char *tok_ctx = NULL, *key = strtok_r(copy, "+", &tok_ctx);
         key != NULL;
         key = strtok_r(NULL, "+", &tok_ctx))
    {
        if (strcmp(key, XKB_MOD_NAME_SHIFT) == 0)
            modifiers->shift = true;
        else if (strcmp(key, XKB_MOD_NAME_CTRL) == 0)
            modifiers->ctrl = true;
        else if (strcmp(key, XKB_MOD_NAME_ALT) == 0)
            modifiers->alt = true;
        else if (strcmp(key, XKB_MOD_NAME_LOGO) == 0)
            modifiers->meta = true;
        else {
            LOG_CONTEXTUAL_ERR("not a valid modifier name: %s", key);
            goto out;
        }
    }

    ret = true;

out:
    free(copy);
    return ret;
}

static bool
value_to_key_combos(struct context *ctx, struct key_combo_list *key_combos)
{
    xassert(key_combos != NULL);
    xassert(key_combos->count == 0 && key_combos->combos == NULL);

    size_t size = 0;  /* Size of ‘combos’ array in the key-combo list */

    char *copy = xstrdup(ctx->value);

    for (char *tok_ctx = NULL, *combo = strtok_r(copy, " ", &tok_ctx);
         combo != NULL;
         combo = strtok_r(NULL, " ", &tok_ctx))
    {
        struct config_key_modifiers modifiers = {0};
        char *key = strrchr(combo, '+');

        if (key == NULL) {
            /* No modifiers */
            key = combo;
        } else {
            if (!parse_modifiers(ctx, combo, key - combo, &modifiers))
                goto err;
            key++;  /* Skip past the '+' */
        }

        /* Translate key name to symbol */
        xkb_keysym_t sym = xkb_keysym_from_name(key, 0);
        if (sym == XKB_KEY_NoSymbol) {
            LOG_CONTEXTUAL_ERR("not a valid XKB key name: %s", key);
            goto err;
        }

        if (key_combos->count + 1 > size) {
            size += 4;
            key_combos->combos = xrealloc(
                key_combos->combos, size * sizeof(key_combos->combos[0]));
        }

        xassert(key_combos->count + 1 <= size);
        key_combos->combos[key_combos->count++] = (struct key_combo){
            .text = xstrdup(combo),
            .modifiers = modifiers,
            .sym = sym,
        };
    }

    free(copy);
    return true;

err:
    free_key_combo_list(key_combos);
    free(copy);
    return false;
}

static bool
has_key_binding_collisions(struct context *ctx,
                           int action, const char *const action_map[],
                           const struct config_key_binding_list *bindings,
                           const struct key_combo_list *key_combos)
{
    for (size_t j = 0; j < bindings->count; j++) {
        const struct config_key_binding *combo1 = &bindings->arr[j];

        if (combo1->action == BIND_ACTION_NONE)
            continue;

        if (combo1->action == action)
            continue;

        for (size_t i = 0; i < key_combos->count; i++) {
            const struct key_combo *combo2 = &key_combos->combos[i];

            const struct config_key_modifiers *mods1 = &combo1->modifiers;
            const struct config_key_modifiers *mods2 = &combo2->modifiers;

            bool shift = mods1->shift == mods2->shift;
            bool alt = mods1->alt == mods2->alt;
            bool ctrl = mods1->ctrl == mods2->ctrl;
            bool meta = mods1->meta == mods2->meta;
            bool sym = combo1->sym == combo2->sym;

            if (shift && alt && ctrl && meta && sym) {
                bool has_pipe = combo1->pipe.argv.args != NULL;
                LOG_CONTEXTUAL_ERR("%s already mapped to '%s%s%s%s'",
                                   combo2->text,
                                   action_map[combo1->action],
                                   has_pipe ? " [" : "",
                                   has_pipe ? combo1->pipe.argv.args[0] : "",
                                   has_pipe ? "]" : "");
                return true;
            }
        }
    }

    return false;
}

static int
argv_compare(char *const *argv1, char *const *argv2)
{
    xassert(argv1 != NULL);
    xassert(argv2 != NULL);

    for (size_t i = 0; ; i++) {
        if (argv1[i] == NULL && argv2[i] == NULL)
            return 0;
        if (argv1[i] == NULL)
            return -1;
        if (argv2[i] == NULL)
            return 1;

        int ret = strcmp(argv1[i], argv2[i]);
        if (ret != 0)
            return ret;
    }

    BUG("unexpected loop break");
    return 1;
}

/*
 * Parses a key binding value on the form
 *  "[cmd-to-exec arg1 arg2] Mods+Key"
 *
 * and extracts 'cmd-to-exec' and its arguments.
 *
 * Input:
 *  - value: raw string, on the form mention above
 *  - cmd: pointer to string to will be allocated and filled with
 *        'cmd-to-exec arg1 arg2'
 *  - argv: point to array of string. Array will be allocated. Will be
 *          filled with {'cmd-to-exec', 'arg1', 'arg2', NULL}
 *
 * Returns:
 *  - ssize_t, number of bytes to strip from 'value' to remove the '[]'
 *    enclosed cmd and its arguments, including any subsequent
 *    whitespace characters. I.e. if 'value' is "[cmd] BTN_RIGHT", the
 *    return value is 6 (strlen("[cmd] ")).
 *  - cmd: allocated string containing "cmd arg1 arg2...". Caller frees.
 *  - argv: allocatd array containing {"cmd", "arg1", "arg2", NULL}. Caller frees.
 */
static ssize_t
pipe_argv_from_value(struct context *ctx, char ***argv)
{
    *argv = NULL;

    if (ctx->value[0] != '[')
        return 0;

    const char *pipe_cmd_end = strrchr(ctx->value, ']');
    if (pipe_cmd_end == NULL) {
        LOG_CONTEXTUAL_ERR("unclosed '['");
        return -1;
    }

    size_t pipe_len = pipe_cmd_end - ctx->value - 1;
    char *cmd = xstrndup(&ctx->value[1], pipe_len);

    if (!tokenize_cmdline(cmd, argv)) {
        LOG_CONTEXTUAL_ERR("syntax error in command line");
        free(cmd);
        return -1;
    }

    ssize_t remove_len = pipe_cmd_end + 1 - ctx->value;
    ctx->value = pipe_cmd_end + 1;
    while (isspace(*ctx->value)) {
        ctx->value++;
        remove_len++;
    }

    free(cmd);
    return remove_len;
}

static void NOINLINE
remove_action_from_key_bindings_list(struct config_key_binding_list *bindings,
                                     int action, char **pipe_argv)
{
    size_t remove_first_idx = 0;
    size_t remove_count = 0;

    for (size_t i = 0; i < bindings->count; i++) {
        struct config_key_binding *binding = &bindings->arr[i];

        if (binding->action == action &&
            ((binding->pipe.argv.args == NULL && pipe_argv == NULL) ||
             (binding->pipe.argv.args != NULL && pipe_argv != NULL &&
              argv_compare(binding->pipe.argv.args, pipe_argv) == 0)))
        {
            if (remove_count++ == 0)
                remove_first_idx = i;

            xassert(remove_first_idx + remove_count - 1 == i);

            if (binding->pipe.master_copy)
                free_argv(&binding->pipe.argv);
        }
    }

    if (remove_count == 0)
        return;

    size_t move_count = bindings->count - (remove_first_idx + remove_count);

    memmove(
        &bindings->arr[remove_first_idx],
        &bindings->arr[remove_first_idx + remove_count],
        move_count * sizeof(bindings->arr[0]));
    bindings->count -= remove_count;
}

static bool NOINLINE
parse_key_binding_section(struct context *ctx,
                          int action_count,
                          const char *const action_map[static action_count],
                          struct config_key_binding_list *bindings)
{
    char **pipe_argv;

    ssize_t pipe_remove_len = pipe_argv_from_value(ctx, &pipe_argv);
    if (pipe_remove_len < 0)
        return false;

    ctx->value += pipe_remove_len;

    for (int action = 0; action < action_count; action++) {
        if (action_map[action] == NULL)
            continue;

        if (strcmp(ctx->key, action_map[action]) != 0)
            continue;

        /* Unset binding */
        if (strcasecmp(ctx->value, "none") == 0) {
            remove_action_from_key_bindings_list(bindings, action, pipe_argv);
            free(pipe_argv);
            return true;
        }

        struct key_combo_list key_combos = {0};
        if (!value_to_key_combos(ctx, &key_combos) ||
            has_key_binding_collisions(
                ctx, action, action_map, bindings, &key_combos))
        {
            free(pipe_argv);
            free_key_combo_list(&key_combos);
            return false;
        }

        remove_action_from_key_bindings_list(bindings, action, pipe_argv);

        /* Emit key bindings */
        size_t ofs = bindings->count;
        bindings->count += key_combos.count;
        bindings->arr = xrealloc(
            bindings->arr, bindings->count * sizeof(bindings->arr[0]));

        bool first = true;
        for (size_t i = 0; i < key_combos.count; i++) {
            const struct key_combo *combo = &key_combos.combos[i];
            struct config_key_binding binding = {
                .action = action,
                .modifiers = combo->modifiers,
                .sym = combo->sym,
                .pipe = {
                    .argv = {
                        .args = pipe_argv,
                    },
                    .master_copy = first,
                },
            };

            /* TODO: we could re-use free:d slots */
            bindings->arr[ofs + i] = binding;
            first = false;
        }

        free_key_combo_list(&key_combos);
        return true;
    }

    LOG_CONTEXTUAL_ERR("not a valid action: %s", ctx->key);
    free(pipe_argv);
    return false;
}

UNITTEST
{
    enum test_actions {
        TEST_ACTION_NONE,
        TEST_ACTION_FOO,
        TEST_ACTION_BAR,
        TEST_ACTION_COUNT,
    };

    const char *const map[] = {
        [TEST_ACTION_NONE] = NULL,
        [TEST_ACTION_FOO] = "foo",
        [TEST_ACTION_BAR] = "bar",
    };

    struct config conf = {0};
    struct config_key_binding_list bindings = {0};

    struct context ctx = {
        .conf = &conf,
        .section = "",
        .key = "foo",
        .value = "Escape",
        .path = "",
    };

    /*
     * ADD foo=Escape
     *
     * This verifies we can bind a single key combo to an action.
     */
    xassert(parse_key_binding_section(&ctx, ALEN(map), map, &bindings));
    xassert(bindings.count == 1);
    xassert(bindings.arr[0].action == TEST_ACTION_FOO);
    xassert(bindings.arr[0].sym == XKB_KEY_Escape);

    /*
     * ADD bar=Control+g Control+Shift+x
     *
     * This verifies we can bind multiple key combos to an action.
     */
    ctx.key = "bar";
    ctx.value = "Control+g Control+Shift+x";
    xassert(parse_key_binding_section(&ctx, ALEN(map), map, &bindings));
    xassert(bindings.count == 3);
    xassert(bindings.arr[0].action == TEST_ACTION_FOO);
    xassert(bindings.arr[1].action == TEST_ACTION_BAR);
    xassert(bindings.arr[1].sym == XKB_KEY_g);
    xassert(bindings.arr[1].modifiers.ctrl);
    xassert(bindings.arr[2].action == TEST_ACTION_BAR);
    xassert(bindings.arr[2].sym == XKB_KEY_x);
    xassert(bindings.arr[2].modifiers.ctrl && bindings.arr[2].modifiers.shift);

    /*
     * REPLACE foo with foo=Mod+v Shift+q
     *
     * This verifies we can update a single-combo action with multiple
     * key combos.
     */
    ctx.key = "foo";
    ctx.value = "Mod1+v Shift+q";
    xassert(parse_key_binding_section(&ctx, ALEN(map), map, &bindings));
    xassert(bindings.count == 4);
    xassert(bindings.arr[0].action == TEST_ACTION_BAR);
    xassert(bindings.arr[1].action == TEST_ACTION_BAR);
    xassert(bindings.arr[2].action == TEST_ACTION_FOO);
    xassert(bindings.arr[2].sym == XKB_KEY_v);
    xassert(bindings.arr[2].modifiers.alt);
    xassert(bindings.arr[3].action == TEST_ACTION_FOO);
    xassert(bindings.arr[3].sym == XKB_KEY_q);
    xassert(bindings.arr[3].modifiers.shift);

    /*
     * REMOVE bar
     */
    ctx.key = "bar";
    ctx.value = "none";
    xassert(parse_key_binding_section(&ctx, ALEN(map), map, &bindings));
    xassert(bindings.count == 2);
    xassert(bindings.arr[0].action == TEST_ACTION_FOO);
    xassert(bindings.arr[1].action == TEST_ACTION_FOO);

    /*
     * REMOVE foo
     */
    ctx.key = "foo";
    ctx.value = "none";
    xassert(parse_key_binding_section(&ctx, ALEN(map), map, &bindings));
    xassert(bindings.count == 0);

    free(bindings.arr);
}

static bool
parse_section_key_bindings(struct context *ctx)
{
    return parse_key_binding_section(
        ctx,
        BIND_ACTION_KEY_COUNT, binding_action_map,
        &ctx->conf->bindings.key);
}

static bool
parse_section_search_bindings(struct context *ctx)
{
    static const char *const search_binding_action_map[] = {
        [BIND_ACTION_SEARCH_NONE] = NULL,
        [BIND_ACTION_SEARCH_CANCEL] = "cancel",
        [BIND_ACTION_SEARCH_COMMIT] = "commit",
        [BIND_ACTION_SEARCH_FIND_PREV] = "find-prev",
        [BIND_ACTION_SEARCH_FIND_NEXT] = "find-next",
        [BIND_ACTION_SEARCH_EDIT_LEFT] = "cursor-left",
        [BIND_ACTION_SEARCH_EDIT_LEFT_WORD] = "cursor-left-word",
        [BIND_ACTION_SEARCH_EDIT_RIGHT] = "cursor-right",
        [BIND_ACTION_SEARCH_EDIT_RIGHT_WORD] = "cursor-right-word",
        [BIND_ACTION_SEARCH_EDIT_HOME] = "cursor-home",
        [BIND_ACTION_SEARCH_EDIT_END] = "cursor-end",
        [BIND_ACTION_SEARCH_DELETE_PREV] = "delete-prev",
        [BIND_ACTION_SEARCH_DELETE_PREV_WORD] = "delete-prev-word",
        [BIND_ACTION_SEARCH_DELETE_NEXT] = "delete-next",
        [BIND_ACTION_SEARCH_DELETE_NEXT_WORD] = "delete-next-word",
        [BIND_ACTION_SEARCH_EXTEND_WORD] = "extend-to-word-boundary",
        [BIND_ACTION_SEARCH_EXTEND_WORD_WS] = "extend-to-next-whitespace",
        [BIND_ACTION_SEARCH_CLIPBOARD_PASTE] = "clipboard-paste",
        [BIND_ACTION_SEARCH_PRIMARY_PASTE] = "primary-paste",
    };

    static_assert(ALEN(search_binding_action_map) == BIND_ACTION_SEARCH_COUNT,
                  "search binding action map size mismatch");

    return parse_key_binding_section(
        ctx,
        BIND_ACTION_SEARCH_COUNT, search_binding_action_map,
        &ctx->conf->bindings.search);
}

static bool
parse_section_url_bindings(struct context *ctx)
{
    static const char *const url_binding_action_map[] = {
        [BIND_ACTION_URL_NONE] = NULL,
        [BIND_ACTION_URL_CANCEL] = "cancel",
        [BIND_ACTION_URL_TOGGLE_URL_ON_JUMP_LABEL] = "toggle-url-visible",
    };

    static_assert(ALEN(url_binding_action_map) == BIND_ACTION_URL_COUNT,
                  "URL binding action map size mismatch");

    return parse_key_binding_section(
        ctx,
        BIND_ACTION_URL_COUNT, url_binding_action_map,
        &ctx->conf->bindings.url);
}

static bool
value_to_mouse_combos(struct context *ctx, struct key_combo_list *key_combos)
{
    xassert(key_combos != NULL);
    xassert(key_combos->count == 0 && key_combos->combos == NULL);

    size_t size = 0;  /* Size of the ‘combos’ array in key_combos */

    char *copy = xstrdup(ctx->value);

    for (char *tok_ctx = NULL, *combo = strtok_r(copy, " ", &tok_ctx);
         combo != NULL;
         combo = strtok_r(NULL, " ", &tok_ctx))
    {
        struct config_key_modifiers modifiers = {0};
        char *key = strrchr(combo, '+');

        if (key == NULL) {
            /* No modifiers */
            key = combo;
        } else {
            *key = '\0';
            if (!parse_modifiers(ctx, combo, key - combo, &modifiers))
                goto err;
            if (modifiers.shift) {
                LOG_CONTEXTUAL_ERR("Shift cannot be used in mouse bindings");
                goto err;
            }
            key++;  /* Skip past the '+' */
        }

        size_t count = 1;
        {
            char *_count = strrchr(key, '-');
            if (_count != NULL) {
                *_count = '\0';
                _count++;

                errno = 0;
                char *end;
                unsigned long value = strtoul(_count, &end, 10);
                if (_count[0] == '\0' || *end != '\0' || errno != 0) {
                    if (errno != 0)
                        LOG_CONTEXTUAL_ERRNO("invalid click count: %s", _count);
                    else
                        LOG_CONTEXTUAL_ERR("invalid click count: %s", _count);
                    goto err;
                }
                count = value;
            }
        }

        static const struct {
            const char *name;
            int code;
        } map[] = {
            {"BTN_LEFT", BTN_LEFT},
            {"BTN_RIGHT", BTN_RIGHT},
            {"BTN_MIDDLE", BTN_MIDDLE},
            {"BTN_SIDE", BTN_SIDE},
            {"BTN_EXTRA", BTN_EXTRA},
            {"BTN_FORWARD", BTN_FORWARD},
            {"BTN_BACK", BTN_BACK},
            {"BTN_TASK", BTN_TASK},
        };

        int button = 0;
        for (size_t i = 0; i < ALEN(map); i++) {
            if (strcmp(key, map[i].name) == 0) {
                button = map[i].code;
                break;
            }
        }

        if (button == 0) {
            LOG_CONTEXTUAL_ERR("invalid mouse button name: %s", key);
            goto err;
        }

        struct key_combo new = {
            .text = xstrdup(combo),
            .modifiers = modifiers,
            .m = {
                .button = button,
                .count = count,
            },
        };

        if (key_combos->count + 1 > size) {
            size += 4;
            key_combos->combos = xrealloc(
                key_combos->combos, size * sizeof(key_combos->combos[0]));
        }

        xassert(key_combos->count + 1 <= size);
        key_combos->combos[key_combos->count++] = new;
    }

    free(copy);
    return true;

err:
    free_key_combo_list(key_combos);
    free(copy);
    return false;
}

static bool
has_mouse_binding_collisions(struct context *ctx,
                             const struct key_combo_list *key_combos)
{
    struct config *conf = ctx->conf;

    for (size_t j = 0; j < conf->bindings.mouse.count; j++) {
        const struct config_mouse_binding *combo1 = &conf->bindings.mouse.arr[j];
        if (combo1->action == BIND_ACTION_NONE)
            continue;

        for (size_t i = 0; i < key_combos->count; i++) {
            const struct key_combo *combo2 = &key_combos->combos[i];

            const struct config_key_modifiers *mods1 = &combo1->modifiers;
            const struct config_key_modifiers *mods2 = &combo2->modifiers;

            bool shift = mods1->shift == mods2->shift;
            bool alt = mods1->alt == mods2->alt;
            bool ctrl = mods1->ctrl == mods2->ctrl;
            bool meta = mods1->meta == mods2->meta;
            bool button = combo1->button == combo2->m.button;
            bool count = combo1->count == combo2->m.count;

            if (shift && alt && ctrl && meta && button && count) {
                bool has_pipe = combo1->pipe.argv.args != NULL;
                LOG_CONTEXTUAL_ERR("%s already mapped to '%s%s%s%s'",
                                   combo2->text,
                                   binding_action_map[combo1->action],
                                   has_pipe ? " [" : "",
                                   has_pipe ? combo1->pipe.argv.args[0] : "",
                                   has_pipe ? "]" : "");
                return true;
            }
        }
    }

    return false;
}


static bool
parse_section_mouse_bindings(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;
    const char *value = ctx->value;

    char **pipe_argv;

    ssize_t pipe_remove_len = pipe_argv_from_value(ctx, &pipe_argv);
    if (pipe_remove_len < 0)
        return false;

    value += pipe_remove_len;

    for (enum bind_action_normal action = 0;
         action < BIND_ACTION_COUNT;
         action++)
    {
        if (binding_action_map[action] == NULL)
            continue;

        if (strcmp(key, binding_action_map[action]) != 0)
            continue;

        /* Unset binding */
        if (strcasecmp(value, "none") == 0) {
            for (size_t i = 0; i < conf->bindings.mouse.count; i++) {
                struct config_mouse_binding *binding =
                    &conf->bindings.mouse.arr[i];

                if (binding->action == action) {
                    if (binding->pipe.master_copy)
                        free_argv(&binding->pipe.argv);
                    binding->action = BIND_ACTION_NONE;
                }
            }
            free(pipe_argv);
            return true;
        }

        struct key_combo_list key_combos = {0};
        if (!value_to_mouse_combos(ctx, &key_combos) ||
            has_mouse_binding_collisions(ctx, &key_combos))
        {
            free(pipe_argv);
            free_key_combo_list(&key_combos);
            return false;
        }

        /* Remove existing bindings for this action */
        for (size_t i = 0; i < conf->bindings.mouse.count; i++) {
            struct config_mouse_binding *binding = &conf->bindings.mouse.arr[i];

            if (binding->action == action &&
                ((binding->pipe.argv.args == NULL && pipe_argv == NULL) ||
                 (binding->pipe.argv.args != NULL && pipe_argv != NULL &&
                  argv_compare(binding->pipe.argv.args, pipe_argv) == 0)))
            {
                if (binding->pipe.master_copy)
                    free_argv(&binding->pipe.argv);
                binding->action = BIND_ACTION_NONE;
            }
        }

        /* Emit mouse bindings */
        size_t ofs = conf->bindings.mouse.count;
        conf->bindings.mouse.count += key_combos.count;
        conf->bindings.mouse.arr = xrealloc(
            conf->bindings.mouse.arr,
            conf->bindings.mouse.count * sizeof(conf->bindings.mouse.arr[0]));

        bool first = true;
        for (size_t i = 0; i < key_combos.count; i++) {
            const struct key_combo *combo = &key_combos.combos[i];
            struct config_mouse_binding binding = {
                .action = action,
                .modifiers = combo->modifiers,
                .button = combo->m.button,
                .count = combo->m.count,
                .pipe = {
                    .argv = {
                        .args = pipe_argv,
                    },
                    .master_copy = first,
                },
            };

            conf->bindings.mouse.arr[ofs + i] = binding;
            first = false;
        }

        free_key_combo_list(&key_combos);
        return true;
    }

    LOG_CONTEXTUAL_ERR("not a valid option: %s", key);
    free(pipe_argv);
    return false;
}

static bool
parse_section_tweak(struct context *ctx)
{
    struct config *conf = ctx->conf;
    const char *key = ctx->key;
    const char *value = ctx->value;
    const char *path = ctx->path;
    unsigned lineno = ctx->lineno;

    if (strcmp(key, "scaling-filter") == 0) {
        static const char *filters[] = {
            [FCFT_SCALING_FILTER_NONE] = "none",
            [FCFT_SCALING_FILTER_NEAREST] = "nearest",
            [FCFT_SCALING_FILTER_BILINEAR] = "bilinear",
            [FCFT_SCALING_FILTER_CUBIC] = "cubic",
            [FCFT_SCALING_FILTER_LANCZOS3] = "lanczos3",
            NULL,
        };

        int filter = value_to_enum(ctx, filters);
        if (filter < 0)
            return false;

        conf->tweak.fcft_filter = filter;
        LOG_WARN("tweak: scaling-filter=%s", filters[filter]);
    }

    else if (strcmp(key, "overflowing-glyphs") == 0) {
        if (!value_to_bool(ctx, &conf->tweak.overflowing_glyphs))
            return false;
        if (!conf->tweak.overflowing_glyphs)
            LOG_WARN("tweak: disabled overflowing glyphs");
    }

    else if (strcmp(key, "damage-whole-window") == 0) {
        if (!value_to_bool(ctx, &conf->tweak.damage_whole_window))
            return false;
        if (conf->tweak.damage_whole_window)
            LOG_WARN("tweak: damage whole window");
    }

    else if (strcmp(key, "grapheme-shaping") == 0) {
        if (!value_to_bool(ctx, &conf->tweak.grapheme_shaping))
            return false;

#if !defined(FOOT_GRAPHEME_CLUSTERING)
        if (conf->tweak.grapheme_shaping) {
            LOG_CONTEXTUAL_WARN(
                "foot was not compiled with support for grapheme shaping");
            conf->tweak.grapheme_shaping = false;
        }
#endif

        if (conf->tweak.grapheme_shaping && !conf->can_shape_grapheme) {
            LOG_WARN(
                "fcft was not compiled with support for grapheme shaping");

            /* Keep it enabled though - this will cause us to do
             * grapheme-clustering at least */
        }

        if (!conf->tweak.grapheme_shaping)
            LOG_WARN("tweak: grapheme shaping disabled");
    }

    else if (strcmp(key, "grapheme-width-method") == 0) {
        int method = value_to_enum(
            ctx, (const char *[]){"wcswidth", "double-width", NULL});

        if (method < 0)
            return false;

        conf->tweak.grapheme_width_method = method;
        LOG_WARN("%s:%d [tweak].grapheme-width-method=%s", path, lineno, value);
    }

    else if (strcmp(key, "render-timer") == 0) {
        int mode = value_to_enum(
            ctx, (const char *[]){"none", "osd", "log", "both", NULL});

        switch (mode) {
        case 0:
            conf->tweak.render_timer_osd = false;
            conf->tweak.render_timer_log = false;
            break;

        case 1:
            conf->tweak.render_timer_osd = true;
            conf->tweak.render_timer_log = false;
            break;

        case 2:
            conf->tweak.render_timer_osd = false;
            conf->tweak.render_timer_log = true;
            break;

        case 3:
            conf->tweak.render_timer_osd = true;
            conf->tweak.render_timer_log = true;
            break;

        case -1:
        default:
            return false;
        }
    }

    else if (strcmp(key, "delayed-render-lower") == 0) {
        uint32_t ns;
        if (!value_to_uint32(ctx, 10, &ns))
            return false;

        if (ns > 16666666) {
            LOG_CONTEXTUAL_ERR("timeout must not exceed 16ms");
            return false;
        }

        conf->tweak.delayed_render_lower_ns = ns;
        LOG_WARN("tweak: delayed-render-lower=%u", ns);
    }

    else if (strcmp(key, "delayed-render-upper") == 0) {
        uint32_t ns;
        if (!value_to_uint32(ctx, 10, &ns))
            return false;

        if (ns > 16666666) {
            LOG_CONTEXTUAL_ERR("timeout must not exceed 16ms");
            return false;
        }

        conf->tweak.delayed_render_upper_ns = ns;
        LOG_WARN("tweak: delayed-render-upper=%u", ns);
    }

    else if (strcmp(key, "max-shm-pool-size-mb") == 0) {
        uint32_t mb;
        if (!value_to_uint32(ctx, 10, &mb))
            return false;

        conf->tweak.max_shm_pool_size = min((int32_t)mb * 1024 * 1024, INT32_MAX);
        LOG_WARN("tweak: max-shm-pool-size=%lld bytes",
                 (long long)conf->tweak.max_shm_pool_size);
    }

    else if (strcmp(key, "box-drawing-base-thickness") == 0) {
        if (!value_to_double(ctx, &conf->tweak.box_drawing_base_thickness))
            return false;

        LOG_WARN("tweak: box-drawing-base-thickness=%f",
                 conf->tweak.box_drawing_base_thickness);
    }

    else if (strcmp(key, "box-drawing-solid-shades") == 0) {
        if (!value_to_bool(ctx, &conf->tweak.box_drawing_solid_shades))
            return false;

        if (!conf->tweak.box_drawing_solid_shades)
            LOG_WARN("tweak: box-drawing-solid-shades=%s",
                     conf->tweak.box_drawing_solid_shades ? "yes" : "no");
    }

    else if (strcmp(key, "font-monospace-warn") == 0)
        return value_to_bool(ctx, &conf->tweak.font_monospace_warn);

    else {
        LOG_CONTEXTUAL_ERR("not a valid option: %s", key);
        return false;
    }

    return true;
}

static bool
parse_key_value(char *kv, const char **section, const char **key, const char **value)
{
    /*strip leading whitespace*/
    while (*kv && isspace(*kv))
        ++kv;

    if (section != NULL)
        *section = NULL;
    *key = kv;
    *value = NULL;

    size_t kvlen = strlen(kv);
    for (size_t i = 0; i < kvlen; ++i) {
        if (kv[i] == '.') {
            if (section != NULL && *section == NULL) {
                *section = kv;
                kv[i] = '\0';
                *key = &kv[i + 1];
            }
        } else if (kv[i] == '=') {
            if (section != NULL && *section == NULL)
                *section = "main";
            kv[i] = '\0';
            *value = &kv[i + 1];
            break;
        }
    }
    if (*value == NULL)
        return false;

    /* Strip trailing whitespace from key (leading stripped earlier) */
    {
        xassert(!isspace(**key));

        char *end = (char *)*key + strlen(*key) - 1;
        while (isspace(*end))
            end--;
        *(end + 1) = '\0';
    }

    /* Strip leading+trailing whitespace from valueue */
    {
        while (isspace(**value))
            ++*value;

        if (*value[0] != '\0') {
            char *end = (char *)*value + strlen(*value) - 1;
            while (isspace(*end))
                end--;
            *(end + 1) = '\0';
        }
    }
    return true;
}

enum section {
    SECTION_MAIN,
    SECTION_BELL,
    SECTION_SCROLLBACK,
    SECTION_URL,
    SECTION_COLORS,
    SECTION_CURSOR,
    SECTION_MOUSE,
    SECTION_CSD,
    SECTION_KEY_BINDINGS,
    SECTION_SEARCH_BINDINGS,
    SECTION_URL_BINDINGS,
    SECTION_MOUSE_BINDINGS,
    SECTION_TWEAK,
    SECTION_COUNT,
};

/* Function pointer, called for each key/value line */
typedef bool (*parser_fun_t)(struct context *ctx);

static const struct {
    parser_fun_t fun;
    const char *name;
} section_info[] = {
    [SECTION_MAIN] =            {&parse_section_main, "main"},
    [SECTION_BELL] =            {&parse_section_bell, "bell"},
    [SECTION_SCROLLBACK] =      {&parse_section_scrollback, "scrollback"},
    [SECTION_URL] =             {&parse_section_url, "url"},
    [SECTION_COLORS] =          {&parse_section_colors, "colors"},
    [SECTION_CURSOR] =          {&parse_section_cursor, "cursor"},
    [SECTION_MOUSE] =           {&parse_section_mouse, "mouse"},
    [SECTION_CSD] =             {&parse_section_csd, "csd"},
    [SECTION_KEY_BINDINGS] =    {&parse_section_key_bindings, "key-bindings"},
    [SECTION_SEARCH_BINDINGS] = {&parse_section_search_bindings, "search-bindings"},
    [SECTION_URL_BINDINGS] =    {&parse_section_url_bindings, "url-bindings"},
    [SECTION_MOUSE_BINDINGS] =  {&parse_section_mouse_bindings, "mouse-bindings"},
    [SECTION_TWEAK] =           {&parse_section_tweak, "tweak"},
};

static_assert(ALEN(section_info) == SECTION_COUNT, "section info array size mismatch");

static enum section
str_to_section(const char *str)
{
    for (enum section section = SECTION_MAIN; section < SECTION_COUNT; ++section) {
        if (strcmp(str, section_info[section].name) == 0)
            return section;
    }
    return SECTION_COUNT;
}

static bool
parse_config_file(FILE *f, struct config *conf, const char *path, bool errors_are_fatal)
{
    enum section section = SECTION_MAIN;

    char *_line = NULL;
    size_t count = 0;

#define error_or_continue()                     \
    {                                           \
        if (errors_are_fatal)                   \
            goto err;                           \
        else                                    \
            continue;                           \
    }

    struct context context = {
        .conf = conf,
        .section = "main",
        .path = path,
        .lineno = 0,
        .errors_are_fatal = errors_are_fatal,
    };
    struct context *ctx = &context;  /* For LOG_AND_*() */

    while (true) {
        errno = 0;
        context.lineno++;

        ssize_t ret = getline(&_line, &count, f);

        if (ret < 0) {
            if (errno != 0) {
                LOG_AND_NOTIFY_ERRNO("failed to read from configuration");
                if (errors_are_fatal)
                    goto err;
            }
            break;
        }

        /* Strip leading whitespace */
        char *line = _line;
        {
            while (isspace(*line))
                line++;
            if (line[0] != '\0') {
                char *end = line + strlen(line) - 1;
                while (isspace(*end))
                    end--;
                *(end + 1) = '\0';
            }
        }

        /* Empty line, or comment */
        if (line[0] == '\0' || line[0] == '#')
            continue;

        /* Split up into key/value pair + trailing comment separated by blank */
        char *key_value = line;
        char *comment = line;
        while (comment[0] != '\0') {
            const char c = comment[0];
            comment++;
            if (isblank(c) && comment[0] == '#') {
                comment[0] = '\0'; /* Terminate key/value pair */
                comment++;
                break;
            }
        }

        /* Check for new section */
        if (key_value[0] == '[') {
            char *end = strchr(key_value, ']');
            if (end == NULL) {
                LOG_CONTEXTUAL_ERR("syntax error: no closing ']'");
                error_or_continue();
            }

            *end = '\0';

            section = str_to_section(&key_value[1]);
            if (section == SECTION_COUNT) {
                LOG_CONTEXTUAL_ERR("invalid section name: %s", &key_value[1]);
                error_or_continue();
            }

            context.section = &key_value[1];

            /* Process next line */
            continue;
        }

        if (section >= SECTION_COUNT) {
            /* Last section name was invalid; ignore all keys in it */
            continue;
        }

        if (!parse_key_value(key_value, NULL, &context.key, &context.value)) {
            LOG_CONTEXTUAL_ERR("syntax error: key/value pair has no value");
            if (errors_are_fatal)
                goto err;
            break;
        }

        LOG_DBG("section=%s, key='%s', value='%s', comment='%s'",
                section_info[section].name, key, value, comment);

        xassert(section >= 0 && section < SECTION_COUNT);

        parser_fun_t section_parser = section_info[section].fun;
        xassert(section_parser != NULL);

        if (!section_parser(ctx))
            error_or_continue();
    }

    free(_line);
    return true;

err:
    free(_line);
    return false;
}

static char *
get_server_socket_path(void)
{
    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime == NULL)
        return xstrdup("/tmp/foot.sock");

    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    if (wayland_display == NULL) {
        return xasprintf("%s/foot.sock", xdg_runtime);
    }

    return xasprintf("%s/foot-%s.sock", xdg_runtime, wayland_display);
}

#define m_none       {0}
#define m_alt        {.alt = true}
#define m_ctrl       {.ctrl = true}
#define m_shift      {.shift = true}
#define m_ctrl_shift {.ctrl = true, .shift = true}

static void
add_default_key_bindings(struct config *conf)
{
    static const struct config_key_binding bindings[] = {
        {BIND_ACTION_SCROLLBACK_UP_PAGE, m_shift, XKB_KEY_Page_Up},
        {BIND_ACTION_SCROLLBACK_DOWN_PAGE, m_shift, XKB_KEY_Page_Down},
        {BIND_ACTION_CLIPBOARD_COPY, m_ctrl_shift, XKB_KEY_c},
        {BIND_ACTION_CLIPBOARD_PASTE, m_ctrl_shift, XKB_KEY_v},
        {BIND_ACTION_PRIMARY_PASTE, m_shift, XKB_KEY_Insert},
        {BIND_ACTION_SEARCH_START, m_ctrl_shift, XKB_KEY_r},
        {BIND_ACTION_FONT_SIZE_UP, m_ctrl, XKB_KEY_plus},
        {BIND_ACTION_FONT_SIZE_UP, m_ctrl, XKB_KEY_equal},
        {BIND_ACTION_FONT_SIZE_UP, m_ctrl, XKB_KEY_KP_Add},
        {BIND_ACTION_FONT_SIZE_DOWN, m_ctrl, XKB_KEY_minus},
        {BIND_ACTION_FONT_SIZE_DOWN, m_ctrl, XKB_KEY_KP_Subtract},
        {BIND_ACTION_FONT_SIZE_RESET, m_ctrl, XKB_KEY_0},
        {BIND_ACTION_FONT_SIZE_RESET, m_ctrl, XKB_KEY_KP_0},
        {BIND_ACTION_SPAWN_TERMINAL, m_ctrl_shift, XKB_KEY_n},
        {BIND_ACTION_SHOW_URLS_LAUNCH, m_ctrl_shift, XKB_KEY_u},
    };

    conf->bindings.key.count = ALEN(bindings);
    conf->bindings.key.arr = xmalloc(sizeof(bindings));
    memcpy(conf->bindings.key.arr, bindings, sizeof(bindings));
}


static void
add_default_search_bindings(struct config *conf)
{
    static const struct config_key_binding bindings[] = {
        {BIND_ACTION_SEARCH_CANCEL, m_ctrl, XKB_KEY_c},
        {BIND_ACTION_SEARCH_CANCEL, m_ctrl, XKB_KEY_g},
        {BIND_ACTION_SEARCH_CANCEL, m_none, XKB_KEY_Escape},
        {BIND_ACTION_SEARCH_COMMIT, m_none, XKB_KEY_Return},
        {BIND_ACTION_SEARCH_FIND_PREV, m_ctrl, XKB_KEY_r},
        {BIND_ACTION_SEARCH_FIND_NEXT, m_ctrl, XKB_KEY_s},
        {BIND_ACTION_SEARCH_EDIT_LEFT, m_none, XKB_KEY_Left},
        {BIND_ACTION_SEARCH_EDIT_LEFT, m_ctrl, XKB_KEY_b},
        {BIND_ACTION_SEARCH_EDIT_LEFT_WORD, m_ctrl, XKB_KEY_Left},
        {BIND_ACTION_SEARCH_EDIT_LEFT_WORD, m_alt, XKB_KEY_b},
        {BIND_ACTION_SEARCH_EDIT_RIGHT, m_none, XKB_KEY_Right},
        {BIND_ACTION_SEARCH_EDIT_RIGHT, m_ctrl, XKB_KEY_f},
        {BIND_ACTION_SEARCH_EDIT_RIGHT_WORD, m_ctrl, XKB_KEY_Right},
        {BIND_ACTION_SEARCH_EDIT_RIGHT_WORD, m_alt, XKB_KEY_f},
        {BIND_ACTION_SEARCH_EDIT_HOME, m_none, XKB_KEY_Home},
        {BIND_ACTION_SEARCH_EDIT_HOME, m_ctrl, XKB_KEY_a},
        {BIND_ACTION_SEARCH_EDIT_END, m_none, XKB_KEY_End},
        {BIND_ACTION_SEARCH_EDIT_END, m_ctrl, XKB_KEY_e},
        {BIND_ACTION_SEARCH_DELETE_PREV, m_none, XKB_KEY_BackSpace},
        {BIND_ACTION_SEARCH_DELETE_PREV_WORD, m_ctrl, XKB_KEY_BackSpace},
        {BIND_ACTION_SEARCH_DELETE_PREV_WORD, m_alt, XKB_KEY_BackSpace},
        {BIND_ACTION_SEARCH_DELETE_NEXT, m_none, XKB_KEY_Delete},
        {BIND_ACTION_SEARCH_DELETE_NEXT_WORD, m_ctrl, XKB_KEY_Delete},
        {BIND_ACTION_SEARCH_DELETE_NEXT_WORD, m_alt, XKB_KEY_d},
        {BIND_ACTION_SEARCH_EXTEND_WORD, m_ctrl, XKB_KEY_w},
        {BIND_ACTION_SEARCH_EXTEND_WORD_WS, m_ctrl_shift, XKB_KEY_w},
        {BIND_ACTION_SEARCH_CLIPBOARD_PASTE, m_ctrl, XKB_KEY_v},
        {BIND_ACTION_SEARCH_CLIPBOARD_PASTE, m_ctrl, XKB_KEY_y},
        {BIND_ACTION_SEARCH_PRIMARY_PASTE, m_shift, XKB_KEY_Insert},
    };

    conf->bindings.search.count = ALEN(bindings);
    conf->bindings.search.arr = xmalloc(sizeof(bindings));
    memcpy(conf->bindings.search.arr, bindings, sizeof(bindings));
}

static void
add_default_url_bindings(struct config *conf)
{
    static const struct config_key_binding bindings[] = {
        {BIND_ACTION_URL_CANCEL, m_ctrl, XKB_KEY_c},
        {BIND_ACTION_URL_CANCEL, m_ctrl, XKB_KEY_g},
        {BIND_ACTION_URL_CANCEL, m_ctrl, XKB_KEY_d},
        {BIND_ACTION_URL_CANCEL, m_none, XKB_KEY_Escape},
        {BIND_ACTION_URL_TOGGLE_URL_ON_JUMP_LABEL, m_none, XKB_KEY_t},
    };

    conf->bindings.url.count = ALEN(bindings);
    conf->bindings.url.arr = xmalloc(sizeof(bindings));
    memcpy(conf->bindings.url.arr, bindings, sizeof(bindings));
}

static void
add_default_mouse_bindings(struct config *conf)
{
    static const struct config_mouse_binding bindings[] = {
        {BIND_ACTION_PRIMARY_PASTE, m_none, BTN_MIDDLE, 1},
        {BIND_ACTION_SELECT_BEGIN, m_none, BTN_LEFT, 1},
        {BIND_ACTION_SELECT_BEGIN_BLOCK, m_ctrl, BTN_LEFT, 1},
        {BIND_ACTION_SELECT_EXTEND, m_none, BTN_RIGHT, 1},
        {BIND_ACTION_SELECT_EXTEND_CHAR_WISE, m_ctrl, BTN_RIGHT, 1},
        {BIND_ACTION_SELECT_WORD, m_none, BTN_LEFT, 2},
        {BIND_ACTION_SELECT_WORD_WS, m_ctrl, BTN_LEFT, 2},
        {BIND_ACTION_SELECT_ROW, m_none, BTN_LEFT, 3},
    };

    conf->bindings.mouse.count = ALEN(bindings);
    conf->bindings.mouse.arr = xmalloc(sizeof(bindings));
    memcpy(conf->bindings.mouse.arr, bindings, sizeof(bindings));
}

static void NOINLINE
config_font_list_clone(struct config_font_list *dst,
                       const struct config_font_list *src)
{
    dst->count = src->count;
    dst->arr = xmalloc(dst->count * sizeof(dst->arr[0]));

    for (size_t j = 0; j < dst->count; j++) {
        dst->arr[j].pt_size = src->arr[j].pt_size;
        dst->arr[j].px_size = src->arr[j].px_size;
        dst->arr[j].pattern = xstrdup(src->arr[j].pattern);
    }
}

bool
config_load(struct config *conf, const char *conf_path,
            user_notifications_t *initial_user_notifications,
            config_override_t *overrides, bool errors_are_fatal)
{
    bool ret = false;
    enum fcft_capabilities fcft_caps = fcft_capabilities();

    *conf = (struct config) {
        .term = xstrdup(FOOT_DEFAULT_TERM),
        .shell = get_shell(),
        .title = xstrdup("foot"),
        .app_id = xstrdup("foot"),
        .word_delimiters = xwcsdup(L",│`|:\"'()[]{}<>"),
        .size = {
            .type = CONF_SIZE_PX,
            .width = 700,
            .height = 500,
        },
        .pad_x = 2,
        .pad_y = 2,
        .resize_delay_ms = 100,
        .bold_in_bright = {
            .enabled = false,
            .palette_based = false,
        },
        .startup_mode = STARTUP_WINDOWED,
        .fonts = {{0}},
        .line_height = {.pt = 0, .px = -1},
        .letter_spacing = {.pt = 0, .px = 0},
        .horizontal_letter_offset = {.pt = 0, .px = 0},
        .vertical_letter_offset = {.pt = 0, .px = 0},
        .use_custom_underline_offset = false,
        .box_drawings_uses_font_glyphs = false,
        .dpi_aware = DPI_AWARE_AUTO, /* DPI-aware when scaling-factor == 1 */
        .bell = {
            .urgent = false,
            .notify = false,
            .command = {
                .argv = {.args = NULL},
            },
            .command_focused = false,
        },
        .url = {
            .label_letters = xwcsdup(L"sadfjklewcmpgh"),
            .uri_characters = xwcsdup(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.,~:;/?#@!$&%*+=\"'()[]"),
            .osc8_underline = OSC8_UNDERLINE_URL_MODE,
        },
        .can_shape_grapheme = fcft_caps & FCFT_CAPABILITY_GRAPHEME_SHAPING,
        .scrollback = {
            .lines = 1000,
            .indicator = {
                .position = SCROLLBACK_INDICATOR_POSITION_RELATIVE,
                .format = SCROLLBACK_INDICATOR_FORMAT_TEXT,
                .text = wcsdup(L""),
            },
            .multiplier = 3.,
        },
        .colors = {
            .fg = default_foreground,
            .bg = default_background,
            .alpha = 0xffff,
            .selection_fg = 0x80000000,  /* Use default bg */
            .selection_bg = 0x80000000,  /* Use default fg */
            .use_custom = {
                .selection = false,
                .jump_label = false,
                .scrollback_indicator = false,
                .url = false,
            },
        },

        .cursor = {
            .style = CURSOR_BLOCK,
            .blink = false,
            .color = {
                .text = 0,
                .cursor = 0,
            },
            .beam_thickness = {.pt = 1.5},
            .underline_thickness = {.pt = 0., .px = -1},
        },
        .mouse = {
            .hide_when_typing = false,
            .alternate_scroll_mode = true,
        },
        .csd = {
            .preferred = CONF_CSD_PREFER_SERVER,
            .font = {0},
            .title_height = 26,
            .border_width = 5,
            .button_width = 26,
        },

        .render_worker_count = sysconf(_SC_NPROCESSORS_ONLN),
        .server_socket_path = get_server_socket_path(),
        .presentation_timings = false,
        .selection_target = SELECTION_TARGET_PRIMARY,
        .hold_at_exit = false,
        .notify = {
            .argv = {.args = NULL},
        },
        .notify_focus_inhibit = true,

        .tweak = {
            .fcft_filter = FCFT_SCALING_FILTER_LANCZOS3,
            .overflowing_glyphs = true,
#if defined(FOOT_GRAPHEME_CLUSTERING) && FOOT_GRAPHEME_CLUSTERING
            .grapheme_shaping = fcft_caps & FCFT_CAPABILITY_GRAPHEME_SHAPING,
#endif
            .grapheme_width_method = GRAPHEME_WIDTH_WCSWIDTH,
            .delayed_render_lower_ns = 500000,         /* 0.5ms */
            .delayed_render_upper_ns = 16666666 / 2,   /* half a frame period (60Hz) */
            .max_shm_pool_size = 512 * 1024 * 1024,
            .render_timer_osd = false,
            .render_timer_log = false,
            .damage_whole_window = false,
            .box_drawing_base_thickness = 0.04,
            .box_drawing_solid_shades = true,
            .font_monospace_warn = true,
        },

        .notifications = tll_init(),
    };

    memcpy(conf->colors.table, default_color_table, sizeof(default_color_table));

    tokenize_cmdline("notify-send -a ${app-id} -i ${app-id} ${title} ${body}",
                     &conf->notify.argv.args);
    tokenize_cmdline("xdg-open ${url}", &conf->url.launch.argv.args);

    static const wchar_t *url_protocols[] = {
        L"http://",
        L"https://",
        L"ftp://",
        L"ftps://",
        L"file://",
        L"gemini://",
        L"gopher://",
    };
    conf->url.protocols = xmalloc(
        ALEN(url_protocols) * sizeof(conf->url.protocols[0]));
    conf->url.prot_count = ALEN(url_protocols);
    conf->url.max_prot_len = 0;

    for (size_t i = 0; i < ALEN(url_protocols); i++) {
        size_t len = wcslen(url_protocols[i]);
        if (len > conf->url.max_prot_len)
            conf->url.max_prot_len = len;
        conf->url.protocols[i] = xwcsdup(url_protocols[i]);
    }

    qsort(
        conf->url.uri_characters,
        wcslen(conf->url.uri_characters),
        sizeof(conf->url.uri_characters[0]),
        &wccmp);

    tll_foreach(*initial_user_notifications, it) {
        tll_push_back(conf->notifications, it->item);
        tll_remove(*initial_user_notifications, it);
    }

    add_default_key_bindings(conf);
    add_default_search_bindings(conf);
    add_default_url_bindings(conf);
    add_default_mouse_bindings(conf);

    struct config_file conf_file = {.path = NULL, .fd = -1};
    if (conf_path != NULL) {
        int fd = open(conf_path, O_RDONLY);
        if (fd < 0) {
            LOG_AND_NOTIFY_ERRNO("%s: failed to open", conf_path);
            ret = !errors_are_fatal;
            goto out;
        }

        conf_file.path = xstrdup(conf_path);
        conf_file.fd = fd;
    } else {
        conf_file = open_config();
        if (conf_file.fd < 0) {
            LOG_WARN("no configuration found, using defaults");
            ret = !errors_are_fatal;
            goto out;
        }
    }

    xassert(conf_file.path != NULL);
    xassert(conf_file.fd >= 0);
    LOG_INFO("loading configuration from %s", conf_file.path);

    FILE *f = fdopen(conf_file.fd, "r");
    if (f == NULL) {
        LOG_AND_NOTIFY_ERRNO("%s: failed to open", conf_file.path);
        ret = !errors_are_fatal;
        goto out;
    }

    ret = parse_config_file(f, conf, conf_file.path, errors_are_fatal) &&
          config_override_apply(conf, overrides, errors_are_fatal);
    fclose(f);

    conf->colors.use_custom.selection =
        conf->colors.selection_fg >> 24 == 0 &&
        conf->colors.selection_bg >> 24 == 0;

out:
    if (ret && conf->fonts[0].count == 0) {
        struct config_font font;
        if (!config_font_parse("monospace", &font)) {
            LOG_ERR("failed to load font 'monospace' - no fonts installed?");
            ret = false;
        } else {
            conf->fonts[0].count = 1;
            conf->fonts[0].arr = malloc(sizeof(font));
            conf->fonts[0].arr[0] = font;
        }
    }

    if (ret && conf->csd.font.count == 0)
        config_font_list_clone(&conf->csd.font, &conf->fonts[0]);

#if defined(_DEBUG)
    for (size_t i = 0; i < conf->bindings.key.count; i++)
        xassert(conf->bindings.key.arr[i].action != BIND_ACTION_NONE);
    for (size_t i = 0; i < conf->bindings.search.count; i++)
        xassert(conf->bindings.search.arr[i].action != BIND_ACTION_SEARCH_NONE);
    for (size_t i = 0; i < conf->bindings.url.count; i++)
        xassert(conf->bindings.url.arr[i].action != BIND_ACTION_URL_NONE);
#endif

    free(conf_file.path);
    if (conf_file.fd >= 0)
        close(conf_file.fd);

    return ret;
}

bool
config_override_apply(struct config *conf, config_override_t *overrides, bool errors_are_fatal)
{
    struct context context = {
        .conf = conf,
        .path = "override",
        .lineno = 0,
        .errors_are_fatal = errors_are_fatal,
    };
    struct context *ctx = &context;

    tll_foreach(*overrides, it) {
        context.lineno++;

        if (!parse_key_value(
                it->item, &context.section, &context.key, &context.value))
        {
            LOG_CONTEXTUAL_ERR("syntax error: key/value pair has no value");
            if (errors_are_fatal)
                return false;
            continue;
        }

        enum section section = str_to_section(context.section);
        if (section == SECTION_COUNT) {
            LOG_CONTEXTUAL_ERR("invalid section name: %s", context.section);
            if (errors_are_fatal)
                return false;
            continue;
        }
        parser_fun_t section_parser = section_info[section].fun;
        xassert(section_parser != NULL);

        if (!section_parser(ctx)) {
            if (errors_are_fatal)
                return false;
            continue;
        }
    }
    return true;
}

static void
binding_pipe_free(struct config_binding_pipe *pipe)
{
    if (pipe->master_copy)
        free_argv(&pipe->argv);
}

static void
binding_pipe_clone(struct config_binding_pipe *dst,
                   const struct config_binding_pipe *src)
{
    xassert(src->master_copy);
    clone_argv(&dst->argv, &src->argv);
}

static void NOINLINE
key_binding_list_free(struct config_key_binding_list *bindings)
{
    for (size_t i = 0; i < bindings->count; i++)
        binding_pipe_free(&bindings->arr[i].pipe);
    free(bindings->arr);
}

static void NOINLINE
key_binding_list_clone(struct config_key_binding_list *dst,
                       const struct config_key_binding_list *src)
{
    struct argv *last_master_argv = NULL;

    dst->count = src->count;
    dst->arr = xmalloc(src->count * sizeof(dst->arr[0]));

    for (size_t i = 0; i < src->count; i++) {
        const struct config_key_binding *old = &src->arr[i];
        struct config_key_binding *new = &dst->arr[i];

        *new = *old;

        if (old->pipe.argv.args == NULL)
            continue;

        if (old->pipe.master_copy) {
            binding_pipe_clone(&new->pipe, &old->pipe);
            last_master_argv = &new->pipe.argv;
        } else {
            xassert(last_master_argv != NULL);
            new->pipe.argv = *last_master_argv;
        }
    }
}

static void
mouse_binding_list_free(struct config_mouse_binding_list *bindings)
{
    for (size_t i = 0; i < bindings->count; i++)
        binding_pipe_free(&bindings->arr[i].pipe);
    free(bindings->arr);
}

static void NOINLINE
mouse_binding_list_clone(struct config_mouse_binding_list *dst,
                         const struct config_mouse_binding_list *src)
{
    struct argv *last_master_argv = NULL;

    dst->count = src->count;
    dst->arr = xmalloc(src->count * sizeof(dst->arr[0]));

    for (size_t i = 0; i < src->count; i++) {
        const struct config_mouse_binding *old = &src->arr[i];
        struct config_mouse_binding *new = &dst->arr[i];

        *new = *old;

        if (old->pipe.argv.args == NULL)
            continue;

        if (old->pipe.master_copy) {
            binding_pipe_clone(&new->pipe, &old->pipe);
            last_master_argv = &new->pipe.argv;
        } else {
            xassert(last_master_argv != NULL);
            new->pipe.argv = *last_master_argv;
        }
    }
}

struct config *
config_clone(const struct config *old)
{
    struct config *conf = xmalloc(sizeof(*conf));
    *conf = *old;

    conf->term = xstrdup(old->term);
    conf->shell = xstrdup(old->shell);
    conf->title = xstrdup(old->title);
    conf->app_id = xstrdup(old->app_id);
    conf->word_delimiters = xwcsdup(old->word_delimiters);
    conf->scrollback.indicator.text = xwcsdup(old->scrollback.indicator.text);
    conf->server_socket_path = xstrdup(old->server_socket_path);
    spawn_template_clone(&conf->bell.command, &old->bell.command);
    spawn_template_clone(&conf->notify, &old->notify);

    for (size_t i = 0; i < ALEN(conf->fonts); i++)
        config_font_list_clone(&conf->fonts[i], &old->fonts[i]);
    config_font_list_clone(&conf->csd.font, &old->csd.font);

    conf->url.label_letters = xwcsdup(old->url.label_letters);
    conf->url.uri_characters = xwcsdup(old->url.uri_characters);
    spawn_template_clone(&conf->url.launch, &old->url.launch);
    conf->url.protocols = xmalloc(
        old->url.prot_count * sizeof(conf->url.protocols[0]));
    for (size_t i = 0; i < old->url.prot_count; i++)
        conf->url.protocols[i] = xwcsdup(old->url.protocols[i]);

    key_binding_list_clone(&conf->bindings.key, &old->bindings.key);
    key_binding_list_clone(&conf->bindings.search, &old->bindings.search);
    key_binding_list_clone(&conf->bindings.url, &old->bindings.url);
    mouse_binding_list_clone(&conf->bindings.mouse, &old->bindings.mouse);

    conf->notifications.length = 0;
    conf->notifications.head = conf->notifications.tail = 0;
    tll_foreach(old->notifications, it) {
        struct user_notification notif = {
            .kind = it->item.kind,
            .text = xstrdup(it->item.text),
        };
        tll_push_back(conf->notifications, notif);
    }

    return conf;
}

UNITTEST
{
    struct config original;
    user_notifications_t nots = tll_init();
    config_override_t overrides = tll_init();

    bool ret = config_load(&original, "/dev/null", &nots, &overrides, false);
    xassert(ret);

    struct config *clone = config_clone(&original);
    xassert(clone != NULL);
    xassert(clone != &original);

    config_free(original);
    config_free(*clone);
    free(clone);

    tll_free(overrides);
    tll_free(nots);
}

void
config_free(struct config conf)
{
    free(conf.term);
    free(conf.shell);
    free(conf.title);
    free(conf.app_id);
    free(conf.word_delimiters);
    spawn_template_free(&conf.bell.command);
    free(conf.scrollback.indicator.text);
    spawn_template_free(&conf.notify);
    for (size_t i = 0; i < ALEN(conf.fonts); i++)
        config_font_list_destroy(&conf.fonts[i]);
    free(conf.server_socket_path);

    config_font_list_destroy(&conf.csd.font);

    free(conf.url.label_letters);
    spawn_template_free(&conf.url.launch);
    for (size_t i = 0; i < conf.url.prot_count; i++)
        free(conf.url.protocols[i]);
    free(conf.url.protocols);
    free(conf.url.uri_characters);

    key_binding_list_free(&conf.bindings.key);
    key_binding_list_free(&conf.bindings.search);
    key_binding_list_free(&conf.bindings.url);
    mouse_binding_list_free(&conf.bindings.mouse);

    user_notifications_free(&conf.notifications);
}

bool
config_font_parse(const char *pattern, struct config_font *font)
{
    FcPattern *pat = FcNameParse((const FcChar8 *)pattern);
    if (pat == NULL)
        return false;

    double pt_size = -1.0;
    FcPatternGetDouble(pat, FC_SIZE, 0, &pt_size);
    FcPatternRemove(pat, FC_SIZE, 0);

    int px_size = -1;
    FcPatternGetInteger(pat, FC_PIXEL_SIZE, 0, &px_size);
    FcPatternRemove(pat, FC_PIXEL_SIZE, 0);

    if (pt_size == -1. && px_size == -1)
        pt_size = 8.0;

    char *stripped_pattern = (char *)FcNameUnparse(pat);
    FcPatternDestroy(pat);

    *font = (struct config_font){
        .pattern = stripped_pattern,
        .pt_size = pt_size,
        .px_size = px_size
    };
    return true;
}

void
config_font_list_destroy(struct config_font_list *font_list)
{
    for (size_t i = 0; i < font_list->count; i++)
        free(font_list->arr[i].pattern);
    free(font_list->arr);
    font_list->count = 0;
    font_list->arr = NULL;
}


bool
check_if_font_is_monospaced(const char *pattern,
                            user_notifications_t *notifications)
{
    struct fcft_font *f = fcft_from_name(
        1, (const char *[]){pattern}, ":size=8");

    if (f == NULL)
        return true;

    static const wchar_t chars[] = {L'a', L'i', L'l', L'M', L'W'};

    bool is_monospaced = true;
    int last_width = -1;

    for (size_t i = 0; i < sizeof(chars) / sizeof(chars[0]); i++) {
        const struct fcft_glyph *g = fcft_glyph_rasterize(
            f, chars[i], FCFT_SUBPIXEL_NONE);

        if (g == NULL)
            continue;

        if (last_width >= 0 && g->advance.x != last_width) {
            LOG_WARN("%s: font does not appear to be monospace; "
                     "check your config, or disable this warning by "
                     "setting [tweak].font-monospace-warn=no",
                     pattern);

            user_notification_add(
                notifications,
                USER_NOTIFICATION_WARNING,
                "%s: font does not appear to be monospace; "
                "check your config, or disable this warning by "
                "setting \033[1m[tweak].font-monospace-warn=no\033[22m",
                pattern);

            is_monospaced = false;
            break;
        }

        last_width = g->advance.x;
    }

    fcft_destroy(f);
    return is_monospaced;
}

#include "csi.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#if defined(_DEBUG)
 #include <stdio.h>
#endif

#define LOG_MODULE "csi"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "grid.h"

static bool
csi_sgr(struct terminal *term)
{
    for (size_t i = 0; i < term->vt.params.idx; i++) {
        switch (term->vt.params.v[i].value) {
        case 0:
            term->vt.attrs.bold = false;
            term->vt.dim = false;
            term->vt.attrs.italic = false;
            term->vt.attrs.underline = false;
            term->vt.attrs.strikethrough = false;
            term->vt.attrs.blink = false;
            term->vt.attrs.conceal = false;
            term->vt.attrs.reverse = false;
            term->vt.attrs.foreground = term->grid.foreground;
            term->vt.attrs.background = term->grid.background;
            break;

        case 1: term->vt.attrs.bold = true; break;
        case 2: term->vt.dim = true; break;
        case 3: term->vt.attrs.italic = true; break;
        case 4: term->vt.attrs.underline = true; break;
        case 5: term->vt.attrs.blink = true; break;
        case 6: term->vt.attrs.blink = true; break;
        case 7: term->vt.attrs.reverse = true; break;
        case 8: term->vt.attrs.conceal = true; break;
        case 9: term->vt.attrs.strikethrough = true; break;

        case 22: term->vt.attrs.bold = term->vt.dim = false; break;
        case 23: term->vt.attrs.italic = false; break;
        case 24: term->vt.attrs.underline = false; break;
        case 25: term->vt.attrs.blink = false; break;
        case 27: term->vt.attrs.reverse = false; break;
        case 28: term->vt.attrs.conceal = false; break;
        case 29: term->vt.attrs.strikethrough = false; break;

        /* Regular foreground colors */
        case 30: term->vt.attrs.foreground = 0x000000ff; break;
        case 31: term->vt.attrs.foreground = 0xff0000ff; break;
        case 32: term->vt.attrs.foreground = 0x00ff00ff; break;
        case 33: term->vt.attrs.foreground = 0xf0f000ff; break;
        case 34: term->vt.attrs.foreground = 0x0000ffff; break;
        case 35: term->vt.attrs.foreground = 0xf000f0ff; break;
        case 36: term->vt.attrs.foreground = 0x00f0f0ff; break;
        case 37: term->vt.attrs.foreground = 0xffffffff; break;
        case 39: term->vt.attrs.foreground = term->grid.foreground; break;

        /* Regular background colors */
        case 40: term->vt.attrs.background = 0x000000ff; break;
        case 41: term->vt.attrs.background = 0xff0000ff; break;
        case 42: term->vt.attrs.background = 0x00ff00ff; break;
        case 43: term->vt.attrs.background = 0xf0f000ff; break;
        case 44: term->vt.attrs.background = 0x0000ffff; break;
        case 45: term->vt.attrs.background = 0xf000f0ff; break;
        case 46: term->vt.attrs.background = 0x00f0f0ff; break;
        case 47: term->vt.attrs.background = 0xffffffff; break;
        case 49: term->vt.attrs.background = term->grid.background; break;

        /* Bright foreground colors */
        /*  TODO: using regular colors for now */
        case 90: term->vt.attrs.foreground = 0x000000ff; break;
        case 91: term->vt.attrs.foreground = 0xff0000ff; break;
        case 92: term->vt.attrs.foreground = 0x00ff00ff; break;
        case 93: term->vt.attrs.foreground = 0xf0f000ff; break;
        case 94: term->vt.attrs.foreground = 0x0000ffff; break;
        case 95: term->vt.attrs.foreground = 0xf000f0ff; break;
        case 96: term->vt.attrs.foreground = 0x00f0f0ff; break;
        case 97: term->vt.attrs.foreground = 0xffffffff; break;

        /* Regular background colors */
        /* TODO: using regular colors for now */
        case 100: term->vt.attrs.background = 0x000000ff; break;
        case 101: term->vt.attrs.background = 0xff0000ff; break;
        case 102: term->vt.attrs.background = 0x00ff00ff; break;
        case 103: term->vt.attrs.background = 0xf0f000ff; break;
        case 104: term->vt.attrs.background = 0x0000ffff; break;
        case 105: term->vt.attrs.background = 0xf000f0ff; break;
        case 106: term->vt.attrs.background = 0x00f0f0ff; break;
        case 107: term->vt.attrs.background = 0xffffffff; break;

        default:
            LOG_ERR("unimplemented: CSI: SGR: %u", term->vt.params.v[i].value);
            return false;
        }
    }

    return true;
}

bool
csi_dispatch(struct terminal *term, uint8_t final)
{
#if defined(_DEBUG)
    char log[1024];
    int c = snprintf(log, sizeof(log), "CSI: ");

    for (size_t i = 0; i < term->vt.params.idx; i++){
        c += snprintf(&log[c], sizeof(log) - c, "%d",
                      term->vt.params.v[i].value);

        for (size_t j = 0; i < term->vt.params.v[i].sub.idx; j++) {
            c += snprintf(&log[c], sizeof(log) - c, ":%d",
                          term->vt.params.v[i].sub.value[j]);
        }

        c += snprintf(&log[c], sizeof(log) - c, "%s",
                      i == term->vt.params.idx - 1 ? "" : ";");
    }

    c += snprintf(&log[c], sizeof(log) - c, "%c (%zu parameters)",
                  final, term->vt.params.idx);
    LOG_DBG("%s", log);
#endif

    if (term->vt.intermediates.idx == 0) {
        switch (final) {
        case 'c':
            return write(term->ptmx, "\033[?6c", 5) == 5;

        case 'm':
            return csi_sgr(term);

        case 'J': {
            /* Erase screen */

            int param = 0;
            if (term->vt.params.idx >= 1)
                param = term->vt.params.v[0].value;

            int start = -1;
            int end = -1;
            switch (param) {
            case 0:
                /* From cursor to end of screen */
                start = term->grid.linear_cursor;
                end = term->grid.cols * term->grid.rows;
                break;

            case 1:
                /* From start of screen to cursor */
                start = 0;
                end = term->grid.linear_cursor;
                break;

            case 2:
                /* Erase entire screen */
                start = 0;
                end = term->grid.cols * term->grid.rows;
                break;

            default:
                LOG_ERR("CSI: J: invalid argument: %d", param);
                return false;
            }

            grid_erase(&term->grid, start, end);
            break;
        }

        case 'K': {
            /* Erase line */

            int param = 0;
            if (term->vt.params.idx >= 0)
                param = term->vt.params.v[0].value;

            int start = -1;
            int end = -1;
            switch (param) {
            case 0:
                /* From cursor to end of line */
                start = term->grid.linear_cursor;
                end = grid_cursor_linear(
                    &term->grid, term->grid.cursor.row, term->grid.cols);
                //LOG_WARN("CSI: K: linear = %d, row = %d, col = %d, end = %d",
                //         start, term->grid.cursor.row, term->grid.cursor.col, end);
                break;

            case 1:
                /* From start of line to cursor */
                start = grid_cursor_linear(
                    &term->grid, term->grid.cursor.row, 0);
                end = term->grid.linear_cursor;
                break;

            case 2:
                /* Entire line */
                start = grid_cursor_linear(
                    &term->grid, term->grid.cursor.row, 0);
                end = grid_cursor_linear(
                    &term->grid, term->grid.cursor.row, term->grid.cols);
                break;

            default:
                LOG_ERR("CSI: K: invalid argument: %d", param);
                return false;
            }

            grid_erase(&term->grid, start, end);
            break;
        }

        case 'A': {
            int count = term->vt.params.idx > 0 ? term->vt.params.v[0].value : 1;
            grid_cursor_up(&term->grid, count);
            //LOG_DBG("CSI: A: row = %d, col = %d", term->grid.cursor.row, term->grid.cursor.col);
            break;
        }

        case 'B': {
            int count = term->vt.params.idx > 0 ? term->vt.params.v[0].value : 1;
            grid_cursor_up(&term->grid, count);
            break;
        }

        case 'C': {
            int count = term->vt.params.idx > 0 ? term->vt.params.v[0].value : 1;
            grid_cursor_right(&term->grid, count);
            //LOG_DBG("CSI: C: row = %d, col = %d", term->grid.cursor.row, term->grid.cursor.col);
            break;
        }

        case 'D': {
            int count = term->vt.params.idx > 0 ? term->vt.params.v[0].value : 1;
            grid_cursor_left(&term->grid, count);
            //LOG_DBG("CSI: D: row = %d, col = %d", term->grid.cursor.row, term->grid.cursor.col);
            break;
        }

        default:
            LOG_ERR("CSI: unimplemented final: %c", final);
            abort();
        }

        return true;
    } else {
        LOG_ERR("CSI: unimplemented: intermediates: %.*s",
                (int)term->vt.intermediates.idx,
                term->vt.intermediates.data);

        //abort();
        return true;
    }

    return false;
}

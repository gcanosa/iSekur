// termbox2 implementation unit (C, so the header compiles as intended).
#define TB_IMPL
#include "termbox2.h"

// Keys sent by terminals in "extended" modes (tmux extended-keys, iTerm2/kitty/
// Ghostty/WezTerm CSI u, xterm modifyOtherKeys) that termbox doesn't know:
//   ESC [ code [; mods] u        ESC [ 27 ; mods ; code ~
// mods-1 bits: 1 shift, 2 alt, 4 ctrl, 8 super (Cmd), 32 meta. Super is
// reported as TB_MOD_SUPER. Registered as the PRE hook so the urxvt mouse
// parser never sees them.
#define TB_MOD_SUPER 16
static int csi_end(size_t *end) {  // index of the final byte of a CSI, if complete
    struct bytebuf_t *in = &global.in;
    if (in->len < 2 || in->buf[1] != '[') return TB_ERR;
    for (size_t i = 2; i < in->len; i++) {
        unsigned char b = (unsigned char)in->buf[i];
        if (b >= 0x40 && b <= 0x7e) { *end = i; return TB_OK; }
        if (b < 0x20 || b > 0x3f) return TB_ERR;  // not a CSI after all
    }
    return TB_ERR_NEED_MORE;
}

int isekur_extract_pre(struct tb_event *ev, size_t *n) {
    struct bytebuf_t *in = &global.in;
    size_t end;
    int rv = csi_end(&end);
    if (rv != TB_OK) return rv == TB_ERR_NEED_MORE && in->len < 32 ? rv : TB_ERR;
    char fin = in->buf[end];
    unsigned v[3] = {0, 1, 0}, nv = 0;
    for (size_t i = 2; i < end && nv < 3; i++) {
        char c = in->buf[i];
        if (c >= '0' && c <= '9') v[nv] = v[nv] * 10 + (unsigned)(c - '0');
        else if (c == ';') v[++nv] = 0;
        else if (c != ':') return TB_ERR;  // kitty sub-fields: keep the first one
        else while (i + 1 < end && in->buf[i + 1] != ';') i++;
    }
    unsigned code, mods;
    if (fin == 'u') code = v[0], mods = nv >= 1 && v[1] ? v[1] - 1 : 0;
    else if (fin == '~' && v[0] == 27 && nv == 2) code = v[2], mods = v[1] ? v[1] - 1 : 0;
    else return TB_ERR;
    ev->type = TB_EVENT_KEY, ev->key = 0, ev->ch = 0, ev->mod = 0;
    if (mods & 1) ev->mod |= TB_MOD_SHIFT;
    if (mods & 34) ev->mod |= TB_MOD_ALT;
    if (mods & 8) ev->mod |= TB_MOD_SUPER;
    if (code == 13) ev->key = TB_KEY_ENTER;
    else if (code == 9) ev->key = TB_KEY_TAB;
    else if (code == 27) ev->key = TB_KEY_ESC;
    else if (code == 127 || code == 8) ev->key = TB_KEY_BACKSPACE2;
    else if (code == 32 && !(mods & 4)) ev->key = TB_KEY_SPACE;
    else if (mods & 4) {  // Ctrl+letter -> the classic control code
        ev->mod |= TB_MOD_CTRL;
        if (code >= 'a' && code <= 'z') ev->key = (uint16_t)(code - 'a' + 1);
        else if (code >= '@' && code <= '_') ev->key = (uint16_t)(code - '@');
        else if (code == ' ') ev->key = TB_KEY_CTRL_2;
    } else if (code >= 32 && code < 0xE000) {  // kitty's private-use codes are keypad/media keys
        ev->ch = (mods & 1) && code >= 'a' && code <= 'z' ? code - 32 : code;
        ev->mod &= (uint8_t)~TB_MOD_SHIFT;
    }
    *n = end + 1;
    return TB_OK;
}

// Any other complete CSI termbox couldn't decode is dropped instead of
// leaking into the input line as "[...".
int isekur_extract_post(struct tb_event *ev, size_t *n) {
    size_t end;
    if (csi_end(&end) != TB_OK) return TB_ERR;
    ev->type = TB_EVENT_KEY, ev->key = 0, ev->ch = 0, ev->mod = 0;
    *n = end + 1;
    return TB_OK;
}

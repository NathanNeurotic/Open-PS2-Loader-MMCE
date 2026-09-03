/*
  Copyright 2009-2010 volca
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#include "include/opl.h"
#include "include/dia.h"
#include "include/gui.h"
#include "include/lang.h"
#include "include/pad.h"
#include "include/renderman.h"
#include "include/fntsys.h"
#include "include/themes.h"
#include "include/util.h"
#include "include/sound.h"

// UI spacing of the dialogues (pixels between consecutive items)
#define UI_SPACING_H          10
#define UI_SETTINGS_SPACING_H 10
#define UI_SPACING_V          2
// spacer ui element width (simulates tab)
#define UI_SPACER_WIDTH       50
// minimal pixel width of spacer
#define UI_SPACER_MINIMAL     30
// length of breaking line in pixels
#define UI_BREAK_LEN          600
// Floor for the dialog repeat delay, in ms. Dialogs are deliberately calmer than the game list --
// overshooting a settings row is more annoying than overshooting a game -- but that intent used to
// be expressed as a HARDCODED 300 ms, which is exactly the "medium" main-menu value. The result was
// that Settings ignored the user's Scroll Speed entirely: picking "fast" (100 ms) still gave 300 ms
// in every dialog (3x slower than asked), and picking "slow" (500 ms) gave no extra slowness at all.
// See diaScrollDelay(); medium is unchanged, so most users see no difference.
#define DIA_SCROLL_MIN_MS     200
// scroll speed (delay in ms!) when setting int value
#define DIA_INT_SET_SPEED     100

static int screenWidth;
static int screenHeight;
static struct UIItem *diaSettingsShellUI;
static const char *diaSettingsIndicator;
static int diaSettingsContext;

// Utility stuff
#define KEYB_MODE   2
#define KEYB_WIDTH  12
#define KEYB_HEIGHT 4
#define KEYB_ITEMS  (KEYB_WIDTH * KEYB_HEIGHT)

static void diaDrawBoundingBox(int x, int y, int w, int h, int focus)
{
    u64 color = focus ? gTheme->selTextColor : gTheme->textColor;

    color |= GS_SETREG_RGBA(0, 0, 0, 0xFF);
    color &= gColFocus;

    rmDrawRect(x - 5, y, w + 10, h + 10, color);
}

int diaShowKeyb(char *text, int maxLen, int hide_text, const char *title)
{
    int i, j, len = strlen(text), selkeyb = 0, x, w;
    int selchar = 0, selcommand = -1;
    // Insertion point, in characters from the start of the string. Starts at the END, so typing and
    // backspace behave exactly as they always did until the user actually moves it (#466).
    //
    // L1/R1 rather than the d-pad: Left/Right already walk the on-screen key grid and hand off to
    // the command column at its edges, which is the primary control on this screen. The request
    // offers the shoulder buttons as its own alternative for that reason.
    int caret = len;
    char c[2] = "\0\0", *mask_buffer;
    static const char keyb0[KEYB_ITEMS] = {
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '\\',
        'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', '`', ':'};

    static const char keyb1[KEYB_ITEMS] = {
        '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '|',
        'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', '~', '.'};
    const char *keyb = keyb0;

    char *commands[KEYB_HEIGHT] = {_l(_STR_BACKSPACE), _l(_STR_SPACE), _l(_STR_ENTER), _l(_STR_MODE)};
    GSTEXTURE *cmdicons[KEYB_HEIGHT];
    cmdicons[0] = thmGetTexture(SQUARE_ICON);
    cmdicons[1] = thmGetTexture(TRIANGLE_ICON);
    cmdicons[2] = thmGetTexture(START_ICON);
    cmdicons[3] = thmGetTexture(SELECT_ICON);

    rmGetScreenExtents(&screenWidth, &screenHeight);

    if (hide_text) {
        if ((mask_buffer = malloc(maxLen)) != NULL) {
            memset(mask_buffer, '*', len);
            mask_buffer[len] = '\0';
        } else {
            // Allocation failed: fall back to showing the text rather than
            // rendering through a NULL mask buffer.
            hide_text = 0;
        }
    } else {
        mask_buffer = NULL;
    }

    while (1) {
        readPads();

        rmStartFrame();
        if (guiDrawBGSettings() == 0)
            guiDrawBGPlasma();
        rmDrawRect(0, 0, screenWidth, screenHeight, gColDarker);

        // Title
        if (title != NULL) {
            fntRenderString(gTheme->fonts[0], 25, 20, ALIGN_NONE, 0, 0, title, gTheme->textColor);
            // separating line
            rmDrawLine(25, 38, 615, 38, gColWhite);
        }

        // Text
        {
            const char *shown = hide_text ? mask_buffer : text;
            fntRenderString(gTheme->fonts[0], 50, 120, ALIGN_NONE, 0, 0, shown, gTheme->textColor);

            // Caret. Drawn by measuring the substring to its LEFT rather than assuming a character
            // width -- the theme font is proportional, so a fixed advance would drift along the
            // line. Rendered as a thin filled rect so it reads as a caret and not as a typed '|'.
            char pre[maxLen];
            int n = (caret < len) ? caret : len;
            memcpy(pre, shown, n);
            pre[n] = '\0';
            int caretX = 50 + rmUnScaleX(fntCalcDimensions(gTheme->fonts[0], pre));
            rmDrawRect(caretX, 120, 2, UI_SPACING_H, gColWhite);
        }

        // separating line for simpler orientation
        rmDrawLine(25, 138, 615, 138, gColWhite);

        for (j = 0; j < KEYB_HEIGHT; j++) {
            for (i = 0; i < KEYB_WIDTH; i++) {
                c[0] = keyb[i + j * KEYB_WIDTH];

                x = 50 + i * 31;
                w = fntRenderString(gTheme->fonts[0], x, 170 + 3 * UI_SPACING_H * j, ALIGN_NONE, 0, 0, c, gTheme->uiTextColor) - x;
                if ((i + j * KEYB_WIDTH) == selchar)
                    diaDrawBoundingBox(x, 170 + 3 * UI_SPACING_H * j, w, UI_SPACING_H, 0);
            }
        }

        // Commands
        for (i = 0; i < KEYB_HEIGHT; i++) {
            if (cmdicons[i]) {
                int w = (cmdicons[i]->Width * 20) / cmdicons[i]->Height;
                int h = 20;
                rmDrawPixmap(cmdicons[i], 436, 170 + 3 * UI_SPACING_H * i, ALIGN_NONE, w, h, SCALING_RATIO, gDefaultCol, 0);
            }

            x = 477;
            w = fntRenderString(gTheme->fonts[0], x, 170 + 3 * UI_SPACING_H * i, ALIGN_NONE, 0, 0, commands[i], gTheme->uiTextColor) - x;
            if (i == selcommand)
                diaDrawBoundingBox(x, 170 + 3 * UI_SPACING_H * i, w, UI_SPACING_H, 0);
        }

        guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? CROSS_ICON : CIRCLE_ICON, _STR_CANCEL, gTheme->fonts[0], 500, 417, gTheme->selTextColor);

        rmEndFrame();

        if (getKey(KEY_L1)) { // caret left
            if (caret > 0) {
                sfxPlay(SFX_CURSOR);
                caret--;
            }
        } else if (getKey(KEY_R1)) { // caret right
            if (caret < len) {
                sfxPlay(SFX_CURSOR);
                caret++;
            }
        } else if (getKey(KEY_LEFT)) {
            sfxPlay(SFX_CURSOR);
            if (selchar > -1) {
                if (selchar % KEYB_WIDTH)
                    selchar--;
                else {
                    selcommand = selchar / KEYB_WIDTH;
                    selchar = -1;
                }
            } else {
                selchar = (selcommand + 1) * KEYB_WIDTH - 1;
                selcommand = -1;
            }
        } else if (getKey(KEY_RIGHT)) {
            sfxPlay(SFX_CURSOR);
            if (selchar > -1) {
                if ((selchar + 1) % KEYB_WIDTH)
                    selchar++;
                else {
                    selcommand = selchar / KEYB_WIDTH;
                    selchar = -1;
                }
            } else {
                selchar = selcommand * KEYB_WIDTH;
                selcommand = -1;
            }
        } else if (getKey(KEY_UP)) {
            sfxPlay(SFX_CURSOR);
            if (selchar > -1)
                selchar = (selchar + KEYB_ITEMS - KEYB_WIDTH) % KEYB_ITEMS;
            else
                selcommand = (selcommand + KEYB_HEIGHT - 1) % KEYB_HEIGHT;
        } else if (getKey(KEY_DOWN)) {
            sfxPlay(SFX_CURSOR);
            if (selchar > -1)
                selchar = (selchar + KEYB_WIDTH) % KEYB_ITEMS;
            else
                selcommand = (selcommand + 1) % KEYB_HEIGHT;
        } else if (getKeyOn(gSelectButton)) {
            if (len < (maxLen - 1) && selchar > -1) {
                sfxPlay(SFX_CONFIRM);
                // Insert AT the caret: shift the tail (and its NUL) right by one, drop the
                // character in, and follow it. With the caret at the end this is the old append.
                memmove(&text[caret + 1], &text[caret], len - caret + 1);
                text[caret] = keyb[selchar];
                if (mask_buffer != NULL) {
                    mask_buffer[len] = '*';
                    mask_buffer[len + 1] = '\0';
                }

                len++;
                caret++;
            } else if (selcommand == 0) {
                sfxPlay(SFX_CANCEL);
                if (caret > 0) { // BACKSPACE -- removes the character LEFT of the caret
                    memmove(&text[caret - 1], &text[caret], len - caret + 1);
                    len--;
                    caret--;
                    if (mask_buffer != NULL)
                        mask_buffer[len] = '\0';
                }
            } else if (selcommand == 1) {
                sfxPlay(SFX_CONFIRM);
                if (len < (maxLen - 1)) { // SPACE -- inserted at the caret, like any other character
                    memmove(&text[caret + 1], &text[caret], len - caret + 1);
                    text[caret] = ' ';
                    if (mask_buffer != NULL) {
                        mask_buffer[len] = '*';
                        mask_buffer[len + 1] = '\0';
                    }

                    len++;
                    caret++;
                }
            } else if (selcommand == 2) {
                sfxPlay(SFX_CONFIRM);
                if (mask_buffer != NULL)
                    free(mask_buffer);
                return 1; // ENTER
            } else if (selcommand == 3) {
                sfxPlay(SFX_CONFIRM);
                selkeyb = (selkeyb + 1) % KEYB_MODE; // MODE
                if (selkeyb == 0)
                    keyb = keyb0;
                if (selkeyb == 1)
                    keyb = keyb1;
            }
        } else if (getKey(KEY_SQUARE)) {
            if (caret > 0) { // BACKSPACE -- same caret-relative delete as the command column
                sfxPlay(SFX_CANCEL);
                memmove(&text[caret - 1], &text[caret], len - caret + 1);
                len--;
                caret--;
                if (mask_buffer != NULL)
                    mask_buffer[len] = '\0';
            }
        } else if (getKey(KEY_TRIANGLE)) {
            if (len < (maxLen - 1) && selchar > -1) { // SPACE
                sfxPlay(SFX_CONFIRM);
                memmove(&text[caret + 1], &text[caret], len - caret + 1);
                text[caret] = ' ';
                if (mask_buffer != NULL) {
                    mask_buffer[len] = '*';
                    mask_buffer[len + 1] = '\0';
                }

                len++;
                caret++;
            }
        } else if (getKeyOn(KEY_START)) {
            sfxPlay(SFX_CONFIRM);
            if (mask_buffer != NULL)
                free(mask_buffer);
            return 1; // ENTER
        } else if (getKeyOn(KEY_SELECT)) {
            selkeyb = (selkeyb + 1) % KEYB_MODE; // MODE
            sfxPlay(SFX_CONFIRM);
            if (selkeyb == 0)
                keyb = keyb0;
            if (selkeyb == 1)
                keyb = keyb1;
        }

        if (getKey(gSelectButton == KEY_CIRCLE ? KEY_CROSS : KEY_CIRCLE)) {
            sfxPlay(SFX_CANCEL);
            break;
        }
    }


    if (mask_buffer != NULL)
        free(mask_buffer);

    return 0;
}

/* NUMERIC ENTRY PAD.

   A deliberately small 3x4 pad, not the full diaShowKeyb() grid. Routing int fields through the
   text keyboard technically works -- its top row is 1234567890 -- but it answers "I do not want to
   hold up/down" with "then hunt through 48 keys instead", which is the same problem wearing a hat.

   '-' toggles the sign of the whole entry rather than inserting a character (there is no valid
   entry with a minus anywhere but the front) and is inert, and drawn dim, on an unsigned field.
   '<' is backspace, so the pad is complete on its own without reaching for a shoulder button. */
#define NUMPAD_WIDTH  3
#define NUMPAD_HEIGHT 4
#define NUMPAD_ITEMS  (NUMPAD_WIDTH * NUMPAD_HEIGHT)

static int diaShowNumPad(char *text, int maxLen, int minv, int maxv)
{
    static const char numpad[NUMPAD_ITEMS] = {
        '1', '2', '3',
        '4', '5', '6',
        '7', '8', '9',
        '-', '0', '<'};
    const int allowNegative = (minv < 0);
    int len = strlen(text), sel = 0, i, j, x, w;
    char c[2] = "\0\0";
    char range[32];

    snprintf(range, sizeof(range), "%d - %d", minv, maxv);
    rmGetScreenExtents(&screenWidth, &screenHeight);

    while (1) {
        readPads();

        rmStartFrame();
        if (guiDrawBGSettings() == 0)
            guiDrawBGPlasma();
        rmDrawRect(0, 0, screenWidth, screenHeight, gColDarker);

        // The entry so far, with the accepted range beside it. An octet and a port look identical
        // on screen otherwise, and guessing which one you are in is how you end up with 255 ports.
        fntRenderString(gTheme->fonts[0], 50, 120, ALIGN_NONE, 0, 0, text, gTheme->textColor);
        fntRenderString(gTheme->fonts[0], 400, 120, ALIGN_NONE, 0, 0, range, gTheme->uiTextColor);
        rmDrawLine(25, 138, 615, 138, gColWhite);

        for (j = 0; j < NUMPAD_HEIGHT; j++) {
            for (i = 0; i < NUMPAD_WIDTH; i++) {
                int idx = i + j * NUMPAD_WIDTH;
                u64 col = gTheme->uiTextColor;

                c[0] = numpad[idx];
                if ((c[0] == '-') && !allowNegative)
                    col = gTheme->textColor;

                x = 270 + i * 40;
                w = fntRenderString(gTheme->fonts[0], x, 170 + 3 * UI_SPACING_H * j, ALIGN_NONE, 0, 0, c, col) - x;
                if (idx == sel)
                    diaDrawBoundingBox(x, 170 + 3 * UI_SPACING_H * j, w, UI_SPACING_H, 0);
            }
        }

        guiDrawIconAndText(SQUARE_ICON, _STR_BACKSPACE, gTheme->fonts[0], 50, 417, gTheme->selTextColor);
        guiDrawIconAndText(START_ICON, _STR_ENTER, gTheme->fonts[0], 250, 417, gTheme->selTextColor);
        guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? CROSS_ICON : CIRCLE_ICON, _STR_CANCEL, gTheme->fonts[0], 500, 417, gTheme->selTextColor);

        rmEndFrame();

        // Wrap within the row on left/right and within the column on up/down: on a 3-wide pad the
        // alternative is walking the cursor off 9 to reach 1.
        if (getKey(KEY_LEFT)) {
            sfxPlay(SFX_CURSOR);
            sel = (sel % NUMPAD_WIDTH) ? sel - 1 : sel + NUMPAD_WIDTH - 1;
        } else if (getKey(KEY_RIGHT)) {
            sfxPlay(SFX_CURSOR);
            sel = ((sel + 1) % NUMPAD_WIDTH) ? sel + 1 : sel - NUMPAD_WIDTH + 1;
        } else if (getKey(KEY_UP)) {
            sfxPlay(SFX_CURSOR);
            sel = (sel + NUMPAD_ITEMS - NUMPAD_WIDTH) % NUMPAD_ITEMS;
        } else if (getKey(KEY_DOWN)) {
            sfxPlay(SFX_CURSOR);
            sel = (sel + NUMPAD_WIDTH) % NUMPAD_ITEMS;
        } else if (getKeyOn(gSelectButton)) {
            c[0] = numpad[sel];
            if (c[0] == '<') {
                if (len > 0) {
                    sfxPlay(SFX_CANCEL);
                    text[--len] = '\0';
                }
            } else if (c[0] == '-') {
                if (!allowNegative)
                    sfxPlay(SFX_CANCEL);
                else if (text[0] == '-') {
                    sfxPlay(SFX_CONFIRM);
                    memmove(text, text + 1, len); // len bytes == the remaining chars AND the NUL
                    len--;
                } else if (len < (maxLen - 1)) {
                    sfxPlay(SFX_CONFIRM);
                    memmove(text + 1, text, len + 1);
                    text[0] = '-';
                    len++;
                }
            } else if (len < (maxLen - 1)) {
                sfxPlay(SFX_CONFIRM);
                text[len++] = c[0];
                text[len] = '\0';
            }
        } else if (getKey(KEY_SQUARE)) { // BACKSPACE
            if (len > 0) {
                sfxPlay(SFX_CANCEL);
                text[--len] = '\0';
            }
        } else if (getKeyOn(KEY_TRIANGLE)) { // CLEAR -- the same button that opened the pad
            sfxPlay(SFX_CANCEL);
            len = 0;
            text[0] = '\0';
        } else if (getKeyOn(KEY_START)) {
            sfxPlay(SFX_CONFIRM);
            return 1; // ENTER
        }

        if (getKey(gSelectButton == KEY_CIRCLE ? KEY_CROSS : KEY_CIRCLE)) {
            sfxPlay(SFX_CANCEL);
            return 0;
        }
    }
}

static int colPadSettings[16];

static int diaShowColSel(unsigned char *r, unsigned char *g, unsigned char *b)
{
    int selc = 0;
    int ret = 0;
    unsigned char col[3];

    padStoreSettings(colPadSettings);

    col[0] = *r;
    col[1] = *g;
    col[2] = *b;
    setButtonDelay(KEY_LEFT, 1);
    setButtonDelay(KEY_RIGHT, 1);

    while (1) {
        readPads();

        rmStartFrame();
        if (guiDrawBGSettings() == 0)
            guiDrawBGPlasma();
        rmDrawRect(0, 0, screenWidth, screenHeight, gColDarker);

        // "Color selection"
        fntRenderString(gTheme->fonts[0], 50, 50, ALIGN_NONE, 0, 0, _l(_STR_COLOR_SELECTION), GS_SETREG_RGBA(0x60, 0x60, 0x60, 0x80));

        // 3 bars representing the colors...
        size_t co;
        int x, y;

        for (co = 0; co < 3; ++co) {
            unsigned char cc[3] = {0, 0, 0};
            cc[co] = col[co];

            x = 75;
            y = 75 + co * 25;

            u64 dcol = GS_SETREG_RGBA(cc[0], cc[1], cc[2], 0x80);

            if (selc == co)
                rmDrawRect(x, y, 200, 20, GS_SETREG_RGBA(0x60, 0x60, 0x60, 0x80));
            else
                rmDrawRect(x, y, 200, 20, GS_SETREG_RGBA(0x20, 0x20, 0x20, 0x80));

            rmDrawRect(x + 2, y + 2, 190.0f * (cc[co] * 100 / 255) / 100, 16, dcol);
        }

        // target color itself
        u64 dcol = GS_SETREG_RGBA(col[0], col[1], col[2], 0x80);

        x = 300;
        y = 75;

        rmDrawRect(x, y, 70, 70, GS_SETREG_RGBA(0x60, 0x60, 0x60, 0x80));
        rmDrawRect(x + 5, y + 5, 60, 60, dcol);

        guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? CIRCLE_ICON : CROSS_ICON, _STR_OK, gTheme->fonts[0], 420, 417, gTheme->selTextColor);
        guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? CROSS_ICON : CIRCLE_ICON, _STR_CANCEL, gTheme->fonts[0], 500, 417, gTheme->selTextColor);

        rmEndFrame();

        /*
          Confirm/cancel are tested FIRST, and outside the direction chain.

          The direction arms below use getKey() under setButtonDelay(..., 1), so while a direction
          is physically held getKey() reports true on EVERY frame. With confirm/cancel chained onto
          the same else-if ladder they were unreachable for the entire duration of a hold: press X
          while still leaning on the D-pad and nothing happened. getKeyOn() is edge-triggered, so
          hoisting it cannot steal a repeat from the directions.
        */
        if (getKeyOn(gSelectButton)) {
            sfxPlay(SFX_CONFIRM);
            *r = col[0];
            *g = col[1];
            *b = col[2];
            ret = 1;
            break;
        }
        if (getKeyOn(gSelectButton == KEY_CIRCLE ? KEY_CROSS : KEY_CIRCLE)) {
            sfxPlay(SFX_CANCEL);
            ret = 0;
            break;
        }

        if (getKey(KEY_LEFT)) {
            if (col[selc] > 0) {
                col[selc]--;
                sfxPlay(SFX_CURSOR);
            }
        } else if (getKey(KEY_RIGHT)) {
            if (col[selc] < 255) {
                col[selc]++;
                sfxPlay(SFX_CURSOR);
            }
        } else if (getKey(KEY_UP)) {
            if (selc > 0) {
                selc--;
                sfxPlay(SFX_CURSOR);
            }
        } else if (getKey(KEY_DOWN)) {
            if (selc < 2) {
                selc++;
                sfxPlay(SFX_CURSOR);
            }
        }
    }

    padRestoreSettings(colPadSettings);
    return ret;
}



// ----------------------------------------------------------------------------
// --------------------------- Dialogue handling ------------------------------
// ----------------------------------------------------------------------------
static const char *diaGetLocalisedText(const char *def, int id)
{
    if (id >= 0)
        return _l(id);

    return def;
}

/// returns true if the item is controllable (e.g. a value can be changed on it)
static int diaIsControllable(struct UIItem *ui)
{
    return (ui->enabled && ui->visible && (ui->type >= UI_OK));
}

/// returns true if the given item should be preceded with nextline
static int diaShouldBreakLine(struct UIItem *ui)
{
    return (ui->type == UI_SPLITTER || ui->type == UI_OK || ui->type == UI_BREAK);
}

/// returns true if the given item should be superseded with nextline
static int diaShouldBreakLineAfter(struct UIItem *ui)
{
    return (ui->type == UI_SPLITTER);
}

static void diaDrawHint(int text_id)
{
    char *text = _l(text_id);

    // Size the box to the hint, but never wider than (almost) the full screen, and clamp the left
    // edge so the start is always on-screen (a long hint used to start off the left edge -- #48).
    int boxW = rmUnScaleX(fntCalcDimensions(gTheme->fonts[0], text)) + 10;
    if (boxW > screenWidth - 20)
        boxW = screenWidth - 20;
    int x = screenWidth - boxW - 10;
    if (x < 10)
        x = 10;
    int innerW = boxW - 10;

    // fntRenderString does NOT word-wrap: it lays the string on ONE line and clips at the box edge
    // (only an explicit '\n' starts a new line). So pre-wrap here -- greedily pack words up to innerW,
    // inserting '\n' between lines. Without this a long hint (e.g. the Neutrino args hint) rendered as
    // a single clipped, unreadable line that ran off the box -- the remaining #48 hint complaint.
    char wrapped[384];
    int wlen = 0, lineW = 0, lines = 1;
    int spaceW = rmUnScaleX(fntCalcDimensions(gTheme->fonts[0], " "));
    const char *p = text;
    while (*p) {
        while (*p == ' ') // skip runs of spaces; we re-insert our own single separators
            p++;
        if (!*p)
            break;
        char word[96];
        int k = 0;
        while (*p && *p != ' ' && k < (int)sizeof(word) - 1)
            word[k++] = *p++;
        word[k] = '\0';
        int wordW = rmUnScaleX(fntCalcDimensions(gTheme->fonts[0], word));

        if (lineW > 0 && (lineW + spaceW + wordW) > innerW) { // word doesn't fit -> new line
            if (wlen < (int)sizeof(wrapped) - 1)
                wrapped[wlen++] = '\n';
            lines++;
            lineW = 0;
        } else if (lineW > 0) { // same line -> re-insert the separating space
            if (wlen < (int)sizeof(wrapped) - 1)
                wrapped[wlen++] = ' ';
            lineW += spaceW;
        }
        for (int i = 0; i < k && wlen < (int)sizeof(wrapped) - 1; i++)
            wrapped[wlen++] = word[i];
        lineW += wordW;
    }
    wrapped[wlen] = '\0';

    int boxH = lines * MENU_ITEM_HEIGHT + 10;
    int y = gTheme->usedHeight - 32 - boxH;

    // render hint on the lower side of the screen.
    rmDrawRect(x, y, boxW, boxH, gColDarker);
    fntRenderString(gTheme->fonts[0], x + 5, y + 5, ALIGN_NONE, innerW, boxH - 5, wrapped, gTheme->textColor);
}

/// renders an ui item (either selected or not)
/// sets width and height of the render into the parameters
// The height diaRenderItem WOULD set for this item, computed without drawing. Kept byte-for-byte in
// step with diaRenderItem's *h logic below: the default is the active spacing, UI_SPACER is 0, UI_COLOUR is
// 17, an invisible controllable item is 0 (diaRenderItem early-returns leaving the caller's h=0), and
// any non-zero fixedHeight overrides upward (negative = percent of screenHeight). *h never depends on
// x/y or the text, so this is exact -- used to advance layout past a row the viewport clip skips (#195
// scroll-bleed fix) so the on-screen rows below it keep their true positions.
static int diaItemHeight(struct UIItem *item, int spacingH)
{
    int h;

    if (!item->visible && item->type >= UI_LABEL)
        return 0;

    if (item->type == UI_SPACER)
        h = 0;
    else if (item->type == UI_COLOUR)
        h = 17;
    else
        h = spacingH;

    if (item->fixedHeight != 0) {
        int newSize = (item->fixedHeight < 0) ? item->fixedHeight * screenHeight / -100 : item->fixedHeight;
        if (h < newSize)
            h = newSize;
    }
    return h;
}

static void diaRenderItem(int x, int y, struct UIItem *item, int selected, int haveFocus, int spacingH, int *w, int *h)
{
    // Don't draw controllable items that are not visible.
    if (!item->visible && item->type >= UI_LABEL)
        return;

    *h = spacingH;

    // all texts are rendered up from the given point!
    u64 txtcol;

    if (diaIsControllable(item))
        txtcol = gTheme->uiTextColor;
    else
        txtcol = gTheme->textColor;

    // let's see what do we have here?
    switch (item->type) {
        case UI_TERMINATOR:
            return;

        case UI_HEADER:
            // header: same layout as a label, just rendered in the accent (selected-text) colour
            txtcol = gTheme->selTextColor;
        case UI_BUTTON:
        case UI_LABEL: {
            // width is text length in pixels...
            const char *txt = diaGetLocalisedText(item->label.text, item->label.stringId);
            if (txt && strlen(txt))
                *w = fntRenderString(gTheme->fonts[0], x, y, ALIGN_NONE, 0, 0, txt, txtcol) - x;
            else
                *w = fntRenderString(gTheme->fonts[0], x, y, ALIGN_NONE, 0, 0, _l(_STR_NOT_SET), txtcol) - x;

            break;
        }

        case UI_SPLITTER: {
            // a line. Thanks to the font rendering, we need to shift up by one font line
            *w = 0;                          // nothing to render at all
            int ypos = y - UI_SPACING_V / 2; //  gsFont->CharHeight +

            // to ODD lines
            ypos &= ~1;

            rmDrawLine(x, ypos, x + UI_BREAK_LEN, ypos, gColWhite);
            break;
        }

        case UI_BREAK:
            *w = 0; // nothing to render at all
            break;

        case UI_SPACER: {
            // next column divisible by spacer
            *w = (UI_SPACER_WIDTH - x % UI_SPACER_WIDTH);

            if (*w < UI_SPACER_MINIMAL) {
                x += *w + UI_SPACER_MINIMAL;
                *w += (UI_SPACER_WIDTH - x % UI_SPACER_WIDTH);
            }

            *h = 0;
            break;
        }

        case UI_OK: {
            const char *txt = _l(_STR_OK);
            *w = fntRenderString(gTheme->fonts[0], x, y, ALIGN_NONE, 0, 0, txt, txtcol) - x;
            break;
        }

        case UI_INT: {
            char tmp[10];

            snprintf(tmp, sizeof(tmp), "%d", item->intvalue.current);
            *w = fntRenderString(gTheme->fonts[0], x, y, ALIGN_NONE, 0, 0, tmp, txtcol) - x;
            break;
        }

        case UI_STRING: {
            if (strlen(item->stringvalue.text))
                *w = fntRenderString(gTheme->fonts[0], x, y, ALIGN_NONE, 0, 0, item->stringvalue.text, txtcol) - x;
            else if (item->showDefaultWhenEmpty)
                // Field has a built-in fallback when blank -- show a dim "Default" so it reads as
                // intentional, not unset. The stored value stays empty, so the fallback still fires.
                *w = fntRenderString(gTheme->fonts[0], x, y, ALIGN_NONE, 0, 0, _l(_STR_DEFAULT), GS_SETREG_RGBA(0x60, 0x60, 0x60, 0x80)) - x;
            else
                *w = fntRenderString(gTheme->fonts[0], x, y, ALIGN_NONE, 0, 0, _l(_STR_NOT_SET), txtcol) - x;
            break;
        }

        case UI_PASSWORD: {
            char stars[32];
            int i;
            int len;

            if (strlen(item->stringvalue.text)) {
                len = min(strlen(item->stringvalue.text), sizeof(stars) - 1);
                for (i = 0; i < len; ++i)
                    stars[i] = '*';

                stars[i] = '\0';
                *w = fntRenderString(gTheme->fonts[0], x, y, ALIGN_NONE, 0, 0, stars, txtcol) - x;
            } else
                *w = fntRenderString(gTheme->fonts[0], x, y, ALIGN_NONE, 0, 0, _l(_STR_NOT_SET), txtcol) - x;
            break;
        }

        case UI_BOOL: {
            const char *txtval = _l((item->intvalue.current) ? _STR_ON : _STR_OFF);
            *w = fntRenderString(gTheme->fonts[0], x, y, ALIGN_NONE, 0, 0, txtval, txtcol) - x;
            break;
        }

        case UI_ENUM: {
            // Guard against a corrupt/out-of-range stored index (e.g. a hand-edited cfg): count the
            // NULL-terminated options, then index with a clamped copy so we never read past the array.
            int ecount = 0;
            while (item->intvalue.enumvalues[ecount] != NULL)
                ecount++;
            int eidx = item->intvalue.current;
            if (eidx < 0 || eidx >= ecount)
                eidx = 0;
            const char *tv = (ecount > 0) ? item->intvalue.enumvalues[eidx] : NULL;

            if (!tv)
                tv = _l(_STR_NO_ITEMS);

            *w = fntRenderString(gTheme->fonts[0], x, y, ALIGN_NONE, 0, 0, tv, txtcol) - x;
            break;
        }

        case UI_COLOUR: {
            *w = rmWideScale(25);
            *h = 17;

            // Align to the right
            x -= *w;

            rmDrawRect(x, y + 3, *w, *h, txtcol);
            u64 dcol = GS_SETREG_RGBA(item->colourvalue.r, item->colourvalue.g, item->colourvalue.b, 0x80);
            rmDrawRect(x + 2, y + 5, *w - 4, *h - 4, dcol);

            break;
        }
    }

    if (selected)
        diaDrawBoundingBox(x, y, *w, *h, haveFocus);

    if (item->fixedWidth != 0) {
        int newSize;
        if (item->fixedWidth < 0)
            newSize = item->fixedWidth * screenWidth / -100;
        else
            newSize = item->fixedWidth;
        if (*w < newSize)
            *w = newSize;
    }

    if (item->fixedHeight != 0) {
        int newSize;
        if (item->fixedHeight < 0)
            newSize = item->fixedHeight * screenHeight / -100;
        else
            newSize = item->fixedHeight;
        if (*h < newSize)
            *h = newSize;
    }
}

// Vertical scroll offset for config dialogs taller than the screen. Reset to 0 when a dialog
// opens (diaExecuteDialog); diaRenderUI shifts rendering up by it and re-clamps it each frame
// so the focused item is always brought on-screen (cursor-follow). Stays 0 when content fits.
// Defined further down; the cursor-follow scroll below needs it to detect "focus on the first control".
static struct UIItem *diaGetFirstControl(struct UIItem *ui);

static int diaScrollOffset = 0;

void diaSetSettingsShell(struct UIItem *ui, const char *indicator)
{
    diaSettingsShellUI = ui;
    diaSettingsIndicator = indicator;
}

void diaSetSettingsContext(int enabled)
{
    diaSettingsContext = enabled;
}

/// renders whole ui screen (for given dialog setup)
void diaRenderUI(struct UIItem *ui, short inMenu, struct UIItem *cur, int haveFocus)
{
    if (guiDrawBGSettings() == 0)
        guiDrawBGPlasma();

    int x0 = 20;
    int y0 = 20;
    int settingsShell = (ui == diaSettingsShellUI && diaSettingsIndicator != NULL);
    int settingsContext = settingsShell || diaSettingsContext;
    int spacingH = settingsContext ? UI_SETTINGS_SPACING_H : UI_SPACING_H;

    // render all items (shifted up by the scroll offset for tall dialogs)
    struct UIItem *rc = ui;
    int x = x0, y = y0 - diaScrollOffset, hmax = 0;
    int curTop = y0, curBot = y0; // rendered extent of the focused row, for cursor-follow scroll
    int contentBottom = y0;       // lowest rendered pixel (screen-space), for the maxScroll upper clamp

    while (rc->type != UI_TERMINATOR) {
        int w = 0, h = 0;

        if (diaShouldBreakLine(rc)) {
            x = x0;

            if (hmax > 0)
                y += hmax + spacingH;

            hmax = 0;
        }

        // Viewport clip for scrolled dialogs (FifthFox HW: "scrolled text moves out of the background
        // image ... clearing the entire screen removes the stray text"). diaScrollOffset shifts rows
        // above y0 (and below the hint bar); nothing else stops them drawing there, and since the
        // dialog re-paints its background rather than full-clearing, a row that scrolled outside the
        // background stays behind as stray text. So SKIP entirely any row fully outside the item
        // viewport -- there is then nothing to leave behind. It is an APP-LAYER skip on purpose: a GS
        // scissor cannot survive the 720p/1080i multi-pass render (its per-pass band scissor gets
        // overwritten), so this works in every video mode where a scissor would not.
        //
        // A skipped row is not drawn, but the layout must advance by its EXACT height (diaItemHeight,
        // which mirrors diaRenderItem's *h logic) so the on-screen rows below it keep their true
        // positions and contentBottom/cursor-follow stay correct. Width is irrelevant for an unshown
        // row -- x resets on the next line break -- so w=0. The FOCUSED row is never skipped (the
        // cursor-follow clamp keeps it inside the viewport by construction), so cursor tracking is exact.
        int rowH = diaItemHeight(rc, spacingH);
        int viewBottom = gTheme->usedHeight - 40; // the same bound the scroll clamp below uses
        if (rc != cur && (y + rowH <= y0 || y >= viewBottom)) {
            w = 0;
            h = rowH;
        } else {
            int renderX = x;
            diaRenderItem(renderX, y, rc, rc == cur, haveFocus, spacingH, &w, &h);
        }

        if (rc == cur) {
            curTop = y;
            curBot = y + h;
        }

        if (w > 0)
            x += w + UI_SPACING_V;

        hmax = (h > hmax) ? h : hmax;

        if (y + h > contentBottom)
            contentBottom = y + h; // track content bottom (screen-space) for the maxScroll clamp

        if (diaShouldBreakLineAfter(rc)) {
            x = x0;

            if (hmax > 0)
                y += hmax + spacingH;

            hmax = 0;
        }

        rc++;
    }

    // Cursor-follow scroll: re-clamp the offset so the focused row stays on-screen, above the
    // bottom hint bar. Self-correcting each frame; remains 0 for dialogs that fit (no scroll).
    if (cur != NULL) {
        int visibleBottom = gTheme->usedHeight - 40;
        // Upper bound: never scroll past the content's bottom. contentBottom is screen-space, so undo
        // the current offset to get the content-space extent. Without this, when the cursor lands on
        // the trailing OK row the offset could over-shoot and stay stuck shifted up (#48).
        int maxScroll = (contentBottom + diaScrollOffset) - visibleBottom;
        if (maxScroll < 0)
            maxScroll = 0;
        // Only scroll when a real viewport exists; guards degenerate small-usedHeight themes
        // where visibleBottom <= y0 would let the two edge corrections fight (jitter).
        if (cur == diaGetFirstControl(ui)) {
            // Focus is on the FIRST navigable control: nothing selectable sits above it, only the
            // non-focusable header/title rows. Snap to the very top so the page title is visible,
            // instead of merely pulling the first control up to y0 and leaving the title clipped
            // off the top of the viewport (#48: page stays stuck shifted down at the first element).
            diaScrollOffset = 0;
        } else if (visibleBottom > y0) {
            if (curTop < y0)
                diaScrollOffset -= (y0 - curTop);
            else if (curBot > visibleBottom)
                diaScrollOffset += (curBot - visibleBottom);
        }
        if (diaScrollOffset > maxScroll)
            diaScrollOffset = maxScroll;
        if (diaScrollOffset < 0)
            diaScrollOffset = 0;
    }

    if ((cur != NULL) && (!haveFocus) && (cur->hintId != -1)) {
        diaDrawHint(cur->hintId);
    }

    /* A focused int accepts Triangle for direct entry, and nothing on screen would otherwise say so:
       dialogs have no per-item hint row while focused (diaDrawHint above is skipped in that state),
       so an undocumented binding would be an undiscoverable one. Widen the standard two-hint footer
       to three for exactly as long as the pad is reachable. */
    int intFocus = (cur != NULL) && haveFocus && (cur->type == UI_INT);

    int uiY = gTheme->usedHeight - 32;
    if (settingsContext) {
        int uiHints[3] = {_STR_SELECT, _STR_BACK, _STR_ENTER};
        int uiIcons[3] = {CROSS_ICON, CIRCLE_ICON, TRIANGLE_ICON};
        int uiX = guiAlignSubMenuHints(intFocus ? 3 : 2, uiHints, uiIcons, gTheme->fonts[0], 12, 2);

        if (settingsShell) {
            // Use the same controller button textures as the rest of OPL. Do not introduce a
            // text-only [L1]/[R1] language for Settings; disk themes can override these embedded
            // defaults. Child editors stay in the Settings context but do not show page controls.
            int pageX = x0;
            GSTEXTURE *l1Tex = thmGetTexture(L1_ICON);
            GSTEXTURE *r1Tex = thmGetTexture(R1_ICON);
            int l1W = l1Tex ? (l1Tex->Width * 20) / l1Tex->Height : 0;
            int r1W = r1Tex ? (r1Tex->Width * 20) / r1Tex->Height : 0;

            if (l1Tex && l1Tex->Mem)
                rmDrawPixmap(l1Tex, pageX, uiY + 10, ALIGN_VCENTER, l1W, 20, SCALING_RATIO, gDefaultCol, 0);
            pageX += rmWideScale(l1W) + 8;
            pageX = fntRenderString(gTheme->fonts[0], pageX, uiY + 10, ALIGN_VCENTER, 0, 0, diaSettingsIndicator, gTheme->textColor);
            pageX += 8;
            if (r1Tex && r1Tex->Mem)
                rmDrawPixmap(r1Tex, pageX, uiY + 10, ALIGN_VCENTER, r1W, 20, SCALING_RATIO, gDefaultCol, 0);
        }
        uiX = guiDrawIconAndText(uiIcons[0], uiHints[0], gTheme->fonts[0], uiX, uiY, gTheme->textColor);
        uiX += 12;
        uiX = guiDrawIconAndText(uiIcons[1], uiHints[1], gTheme->fonts[0], uiX, uiY, gTheme->textColor);
        if (intFocus) {
            uiX += 12;
            guiDrawIconAndText(uiIcons[2], uiHints[2], gTheme->fonts[0], uiX, uiY, gTheme->textColor);
        }
    } else {
        int uiHints[3] = {_STR_SELECT, _STR_BACK, _STR_ENTER};
        int uiIcons[3] = {CIRCLE_ICON, CROSS_ICON, TRIANGLE_ICON};
        int uiX = guiAlignSubMenuHints(intFocus ? 3 : 2, uiHints, uiIcons, gTheme->fonts[0], 12, 2);

        uiX = guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? uiIcons[0] : uiIcons[1], uiHints[0], gTheme->fonts[0], uiX, uiY, gTheme->textColor);
        uiX += 12;
        uiX = guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? uiIcons[1] : uiIcons[0], uiHints[1], gTheme->fonts[0], uiX, uiY, gTheme->textColor);
        if (intFocus) {
            // Triangle stays Triangle: the swap above only ever exchanges cross and circle.
            uiX += 12;
            guiDrawIconAndText(uiIcons[2], uiHints[2], gTheme->fonts[0], uiX, uiY, gTheme->textColor);
        }
    }
}

/// sets the ui item value to the default again
static void diaResetValue(struct UIItem *item)
{
    switch (item->type) {
        case UI_INT:
        case UI_BOOL:
        case UI_ENUM:
            item->intvalue.current = item->intvalue.def;
            return;
        case UI_STRING:
        case UI_PASSWORD:
            strncpy(item->stringvalue.text, item->stringvalue.def, sizeof(item->stringvalue.text));
            item->stringvalue.text[sizeof(item->stringvalue.text) - 1] = '\0';
            return;
        default:
            return;
    }
}

static int diaHandleInput(struct UIItem *item, int *modified, int settingsContext)
{
    int selectButton = settingsContext ? KEY_CROSS : gSelectButton;
    int cancelButton = settingsContext ? KEY_CIRCLE : (gSelectButton == KEY_CIRCLE ? KEY_CROSS : KEY_CIRCLE);

    // circle loses focus, sets old values first
    if (getKeyOn(cancelButton)) {
        diaResetValue(item);
        sfxPlay(SFX_CONFIRM);
        return 0;
    }

    // cross loses focus without setting default
    if (getKeyOn(selectButton)) {
        sfxPlay(SFX_CONFIRM);
        *modified = 0;
        return 0;
    }

    // UI item type dependant part:
    if (item->type == UI_BOOL) {
        // a trick. Set new value, lose focus
        item->intvalue.current = !item->intvalue.current;
        return 0;
    }
    if (item->type == UI_INT) {
        /* DIRECT ENTRY. Nudging with up/down is fine for a value near where it already is and awful
           for anything else -- an IP octet or a 65535-max port is not something you hold a direction
           to reach. Triangle opens diaShowNumPad() seeded with the current value.

           Up/down still nudge, deliberately: most int fields here are small ranges (mtap port 1-2,
           turbo speed 1-4, caches 0-32) where one press IS the fastest route, and taking that away
           to fix the octets would just move the annoyance. Triangle is free while an int has focus
           -- dia.c binds it only INSIDE the keyboard, for space -- so this adds a way in without
           taking one away, and cross/circle still accept/cancel exactly as before. */
        if (getKeyOn(KEY_TRIANGLE)) {
            char numbuf[16];
            char *end = NULL;
            long entered;

            snprintf(numbuf, sizeof(numbuf), "%d", item->intvalue.current);
            if (diaShowNumPad(numbuf, sizeof(numbuf), item->intvalue.min, item->intvalue.max)) {
                entered = strtol(numbuf, &end, 10);
                // Reject junk instead of storing it: strtol answers 0 for "abc", which would look
                // like a deliberate entry. Only commit when at least one digit was consumed.
                if (end != numbuf) {
                    if (entered < item->intvalue.min)
                        entered = item->intvalue.min;
                    if (entered > item->intvalue.max)
                        entered = item->intvalue.max;
                    item->intvalue.current = (int)entered;
                }
            }
            return 0; // lose focus after editing, same as the string fields
        }

        // to be sure
        setButtonDelay(KEY_UP, DIA_INT_SET_SPEED);
        setButtonDelay(KEY_DOWN, DIA_INT_SET_SPEED);

        // up and down
        if (getKey(KEY_UP)) {
            sfxPlay(SFX_CURSOR);
            if (item->intvalue.current < item->intvalue.max) {
                item->intvalue.current++;
            } else {
                item->intvalue.current = item->intvalue.min; // was "= 0;"
            }
        } else if (getKey(KEY_DOWN)) {
            sfxPlay(SFX_CURSOR);
            if (item->intvalue.current > item->intvalue.min) {
                item->intvalue.current--;
            } else {
                item->intvalue.current = item->intvalue.max;
            }
        } else
            *modified = 0;
    } else if ((item->type == UI_STRING) || (item->type == UI_PASSWORD)) {
        char tmp[32];
        strncpy(tmp, item->stringvalue.text, sizeof(tmp));
        tmp[sizeof(tmp) - 1] = '\0';

        if (item->stringvalue.handler) {
            if (item->stringvalue.handler(tmp, sizeof(tmp))) {
                strncpy(item->stringvalue.text, tmp, sizeof(item->stringvalue.text));
                item->stringvalue.text[sizeof(item->stringvalue.text) - 1] = '\0';
            }
        } else {
            if (diaShowKeyb(tmp, sizeof(tmp), item->type == UI_PASSWORD, NULL)) {
                strncpy(item->stringvalue.text, tmp, sizeof(item->stringvalue.text));
                item->stringvalue.text[sizeof(item->stringvalue.text) - 1] = '\0';
            }
        }

        return 0;
    } else if (item->type == UI_ENUM) {
        // Snap a corrupt/out-of-range current index back into range before indexing the
        // NULL-terminated enumvalues[] (count first so we never read past the terminator).
        int ecount = 0;
        while (item->intvalue.enumvalues[ecount] != NULL)
            ecount++;
        if (item->intvalue.current < 0 || item->intvalue.current >= ecount)
            item->intvalue.current = 0;

        if (getKey(KEY_UP) && (item->intvalue.current > 0)) {
            item->intvalue.current--;
            sfxPlay(SFX_CURSOR);
        } else if (getKey(KEY_DOWN) && (item->intvalue.enumvalues[item->intvalue.current + 1] != NULL)) {
            item->intvalue.current++;
            sfxPlay(SFX_CURSOR);
        }

        else {
            *modified = 0;
        }

    } else if (item->type == UI_COLOUR) {
        if (!diaShowColSel(&item->colourvalue.r, &item->colourvalue.g, &item->colourvalue.b))
            *modified = 0;

        return 0;
    }

    return 1;
}

static struct UIItem *diaGetFirstControl(struct UIItem *ui)
{
    struct UIItem *cur = ui;

    while (!diaIsControllable(cur)) {
        if (cur->type == UI_TERMINATOR)
            return ui;

        cur++;
    }

    return cur;
}

static struct UIItem *diaGetLastControl(struct UIItem *ui)
{
    struct UIItem *last = diaGetFirstControl(ui);
    struct UIItem *cur = last;

    while (cur->type != UI_TERMINATOR) {
        cur++;

        if (diaIsControllable(cur))
            last = cur;
    }

    return last;
}

static struct UIItem *diaGetNextControl(struct UIItem *cur, struct UIItem *dflt)
{
    while (cur->type != UI_TERMINATOR) {
        cur++;

        if (diaIsControllable(cur))
            return cur;
    }

    return dflt;
}

static struct UIItem *diaGetPrevControl(struct UIItem *cur, struct UIItem *ui)
{
    struct UIItem *newf = cur;

    while (newf != ui) {
        newf--;

        if (diaIsControllable(newf))
            return newf;
    }

    return cur;
}

/// finds first control on previous line...
static struct UIItem *diaGetPrevLine(struct UIItem *cur, struct UIItem *ui)
{
    struct UIItem *newf = cur;

    int lb = 0;
    int hadCtrl = 0; // had the scanned line any control?

    while (newf != ui) {
        newf--;

        if ((lb > 0) && (diaIsControllable(newf)))
            hadCtrl = 1;

        if (diaShouldBreakLine(newf)) { // is this a line break?
            if (hadCtrl || lb == 0) {
                lb++;
                hadCtrl = 0;
            }
        }

        // twice the break? find first control
        if (lb == 2)
            return diaGetFirstControl(newf);
    }

    return cur;
}

static struct UIItem *diaGetNextLine(struct UIItem *cur, struct UIItem *ui)
{
    struct UIItem *newf = cur;

    int lb = 0;

    while (newf->type != UI_TERMINATOR) {
        newf++;

        if (diaShouldBreakLine(newf)) { // is this a line break?
            lb++;
        }

        if (lb == 1)
            return diaGetNextControl(newf, cur);
    }

    return cur;
}

static int diaPadSettings[16];

static void diaStoreScrollSpeed(void)
{
    padStoreSettings(diaPadSettings);
}

static void diaRestoreScrollSpeed(void)
{
    padRestoreSettings(diaPadSettings);
}

/*
  Dialog repeat delay, derived from the user's Scroll Speed instead of ignoring it.

  guiUpdateScrollSpeed() maps gScrollSpeed 0/1/2 -> 500/300/100 ms for the main menu. Dialogs take
  that same value but never go below DIA_SCROLL_MIN_MS, keeping them calmer than the game list
  without overriding the preference outright:

    slow   (500) -> 500   (was 300: "slow" now actually is slower)
    medium (300) -> 300   (unchanged -- the shipped default is unaffected)
    fast   (100) -> 200   (was 300: "fast" is now genuinely faster, with a floor so a settings
                           list cannot become as twitchy as the game list)
*/
static int diaScrollDelay(void)
{
    int delay = 500 - gScrollSpeed * 200; // mirrors guiUpdateScrollSpeed()

    // gScrollSpeed is sanitised to 0..2 by guiUpdateScrollSpeed, but this runs on paths that may
    // not have gone through it yet; clamp rather than trust it.
    if (delay < DIA_SCROLL_MIN_MS)
        delay = DIA_SCROLL_MIN_MS;
    if (delay > 500)
        delay = 500;

    return delay;
}

static struct UIItem *diaFindByID(struct UIItem *ui, int id)
{
    while (ui->type != UI_TERMINATOR) {
        if (ui->id == id)
            return ui;

        ui++;
    }

    return NULL;
}

int diaExecuteDialog(struct UIItem *ui, int uiId, short inMenu, int (*updater)(int modified))
{
    rmGetScreenExtents(&screenWidth, &screenHeight);

    struct UIItem *cur = NULL;
    if (uiId != -1)
        cur = diaFindByID(ui, uiId);

    if (!cur)
        cur = diaGetFirstControl(ui);

    // what? no controllable item? Exit!
    if (!diaIsControllable(cur))
        return -1;

    int haveFocus = 0, modified;

    diaStoreScrollSpeed();

    int settingsShell = diaSettingsShellUI == ui && diaSettingsIndicator != NULL;
    int settingsContext = settingsShell || diaSettingsContext;

    // slower controls for dialogs
    setButtonDelay(KEY_UP, diaScrollDelay());
    setButtonDelay(KEY_DOWN, diaScrollDelay());

    diaScrollOffset = 0; // start each dialog scrolled to the top

    // okay, we have the first selectable item
    // we can proceed with rendering etc. etc.
    while (1) {
        rmStartFrame();
        diaRenderUI(ui, inMenu, cur, haveFocus);
        // NOTE(rebuild): the Debug-Colors HUD line returns with checklist item 46.
        rmEndFrame();

        readPads();

        if (haveFocus) {
            modified = 1;
            haveFocus = diaHandleInput(cur, &modified, settingsContext);

            if (!haveFocus) {
                setButtonDelay(KEY_UP, diaScrollDelay());
                setButtonDelay(KEY_DOWN, diaScrollDelay());
            }
        } else {
            modified = 0;
            struct UIItem *newf = cur;

            // Settings uses L1/R1 as peer-screen navigation. Keep this in the dialog layer so
            // every peer screen retains the same focus, scrolling and modal-editor behavior as
            // existing dialogs. Ordinary dialogs never see these results unless their caller opts
            // into handling them.
            if (settingsShell && getKeyOn(KEY_L1)) {
                diaRestoreScrollSpeed();
                sfxPlay(SFX_CURSOR);
                return DIA_RESULT_PREV;
            }

            if (settingsShell && getKeyOn(KEY_R1)) {
                diaRestoreScrollSpeed();
                sfxPlay(SFX_CURSOR);
                return DIA_RESULT_NEXT;
            }

            if (getKey(KEY_LEFT)) {
                newf = diaGetPrevControl(cur, ui);
                if (newf == cur)
                    newf = diaGetLastControl(ui);
            }

            if (getKey(KEY_RIGHT)) {
                newf = diaGetNextControl(cur, cur);
                if (newf == cur)
                    newf = diaGetFirstControl(ui);
            }

            if (getKey(KEY_UP)) {
                newf = diaGetPrevLine(cur, ui);
                if (newf == cur)
                    newf = diaGetLastControl(ui);
            }

            if (getKey(KEY_DOWN)) {
                newf = diaGetNextLine(cur, ui);
                if (newf == cur)
                    newf = diaGetFirstControl(ui);
            }

            if (newf != cur) {
                // Navigation change detected
                sfxPlay(SFX_CURSOR);
                cur = newf;
            }

            // Cancel button breaks focus or exits with false result
            if (getKeyOn(settingsContext ? KEY_CIRCLE : (gSelectButton == KEY_CIRCLE ? KEY_CROSS : KEY_CIRCLE))) {
                diaRestoreScrollSpeed();
                sfxPlay(SFX_CANCEL);
                return UIID_BTN_CANCEL;
            }

            // see what key events we have
            if (getKeyOn(settingsContext ? KEY_CROSS : gSelectButton)) {
                haveFocus = 1;
                sfxPlay(SFX_CONFIRM);

                if (cur->type == UI_BUTTON) {
                    diaRestoreScrollSpeed();
                    return cur->id;
                }

                if (cur->type == UI_OK) {
                    diaRestoreScrollSpeed();
                    return UIID_BTN_OK;
                }
            }
        }

        if (updater) {
            int updResult = updater(modified);
            if (updResult) {
                // The other three exits (above) restore; this one did not, leaking the dialog's
                // 300 ms KEY_UP/KEY_DOWN delay into whatever screen follows. Inert today -- every
                // settings updater returns 0, and each dialog re-applies the delay on entry -- but
                // it is a real asymmetry, and the next updater that returns non-zero inherits it.
                diaRestoreScrollSpeed();
                return updResult;
            }
        }
    }
}

void diaSetEnabled(struct UIItem *ui, int id, int enabled)
{
    struct UIItem *item = diaFindByID(ui, id);

    if (!item)
        return;

    item->enabled = enabled;
}

void diaSetShowDefaultWhenEmpty(struct UIItem *ui, int id, int show)
{
    struct UIItem *item = diaFindByID(ui, id);

    if (!item)
        return;

    item->showDefaultWhenEmpty = show;
}

void diaSetVisible(struct UIItem *ui, int id, int visible)
{
    struct UIItem *item = diaFindByID(ui, id);

    if (!item)
        return;

    item->visible = visible;
}

void diaSetItemType(struct UIItem *ui, int id, UIItemType type)
{
    struct UIItem *item = diaFindByID(ui, id);

    if (!item)
        return;

    item->type = type;
}

int diaGetInt(struct UIItem *ui, int id, int *value)
{
    struct UIItem *item = diaFindByID(ui, id);

    if (!item)
        return 0;

    if ((item->type == UI_INT) || (item->type == UI_BOOL) || (item->type == UI_ENUM)) {
        *value = item->intvalue.current;
        return 1;
    }

    return 0;
}

int diaSetInt(struct UIItem *ui, int id, int value)
{
    struct UIItem *item = diaFindByID(ui, id);

    if (!item)
        return 0;

    if ((item->type == UI_INT) || (item->type == UI_BOOL) || (item->type == UI_ENUM)) {
        item->intvalue.def = value;
        item->intvalue.current = value;
        return 1;
    }

    return 0;
}

int diaGetString(struct UIItem *ui, int id, char *value, int length)
{
    struct UIItem *item = diaFindByID(ui, id);

    if (!item)
        return 0;

    if ((item->type == UI_STRING) || (item->type == UI_PASSWORD)) {
        strncpy(value, item->stringvalue.text, length);
        if (length > 0)
            value[length - 1] = '\0';
        return 1;
    }

    return 0;
}

int diaSetString(struct UIItem *ui, int id, const char *text)
{
    struct UIItem *item = diaFindByID(ui, id);

    if (!item)
        return 0;

    if ((item->type == UI_STRING) || (item->type == UI_PASSWORD)) {
        strncpy(item->stringvalue.def, text, sizeof(item->stringvalue.def));
        item->stringvalue.def[sizeof(item->stringvalue.def) - 1] = '\0';
        strncpy(item->stringvalue.text, text, sizeof(item->stringvalue.text));
        item->stringvalue.text[sizeof(item->stringvalue.text) - 1] = '\0';
        return 1;
    }

    return 0;
}

int diaGetColor(struct UIItem *ui, int id, unsigned char *col)
{
    struct UIItem *item = diaFindByID(ui, id);

    if (!item)
        return 0;

    if (item->type != UI_COLOUR)
        return 0;

    col[0] = item->colourvalue.r;
    col[1] = item->colourvalue.g;
    col[2] = item->colourvalue.b;
    return 1;
}

int diaSetColor(struct UIItem *ui, int id, const unsigned char *col)
{
    struct UIItem *item = diaFindByID(ui, id);

    if (!item)
        return 0;

    if (item->type != UI_COLOUR)
        return 0;

    item->colourvalue.r = col[0];
    item->colourvalue.g = col[1];
    item->colourvalue.b = col[2];
    return 1;
}

int diaSetU64Color(struct UIItem *ui, int id, u64 col)
{
    struct UIItem *item = diaFindByID(ui, id);

    if (!item)
        return 0;

    if (item->type != UI_COLOUR)
        return 0;

    item->colourvalue.r = col & 0xFF;
    col >>= 8;
    item->colourvalue.g = col & 0xFF;
    col >>= 8;
    item->colourvalue.b = col & 0xFF;
    return 1;
}

int diaSetLabel(struct UIItem *ui, int id, const char *text)
{
    struct UIItem *item = diaFindByID(ui, id);

    if (!item)
        return 0;

    if ((item->type == UI_LABEL) || (item->type == UI_BUTTON) || (item->type == UI_HEADER)) {
        item->label.text = text;
        return 1;
    }

    return 0;
}

// LIFETIME CONTRACT: this stores enumvals' RAW POINTER -- no copy. The array must outlive every
// render of the dialog (dialog structs are also reused across openings). A block-scoped array is
// a dangling pointer the moment its brace closes (issue #154: the DMA row rendered the Neutrino
// video-mode list). Use static arrays for pure literals; _l()-localized arrays must stay
// function-scope in the function that executes the dialog (static would freeze the first language).
int diaSetEnum(struct UIItem *ui, int id, const char **enumvals)
{
    struct UIItem *item = diaFindByID(ui, id);

    if (!item)
        return 0;

    if (item->type != UI_ENUM)
        return 0;

    item->intvalue.enumvalues = enumvals;
    return 1;
}

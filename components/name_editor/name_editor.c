#include "name_editor.h"

#include <string.h>

static const char *ROW_LETTERS_LOWER = "abcdefghijklmnopqrstuvwxyz";
static const char *ROW_LETTERS_UPPER = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char *ROW_SPECIAL       = " .,_-+*/!?@#&()[]";
static const char *ROW_NUMBERS       = "0123456789";

static const char *row_text(const name_editor_t *ed, uint8_t row)
{
    switch (row) {
    case 0: return ed && ed->uppercase ? ROW_LETTERS_UPPER : ROW_LETTERS_LOWER;
    case 1: return ROW_SPECIAL;
    case 2: return ROW_NUMBERS;
    default: return "";
    }
}

void name_editor_open(name_editor_t *ed, const char *initial)
{
    if (!ed) return;
    memset(ed, 0, sizeof(*ed));
    ed->active = true;

    if (initial) {
        strncpy(ed->text, initial, sizeof(ed->text) - 1);
    }
    ed->cursor = (uint8_t)strnlen(ed->text, sizeof(ed->text) - 1);
}

void name_editor_close(name_editor_t *ed)
{
    if (ed) ed->active = false;
}

void name_editor_move_horizontal(name_editor_t *ed, int delta)
{
    if (!ed || !ed->active || delta == 0) return;
    const char *row = row_text(ed, ed->row);
    int len = (int)strlen(row);
    if (len <= 0) return;

    int col = (int)ed->col + delta;
    while (col < 0) col += len;
    while (col >= len) col -= len;
    ed->col = (uint8_t)col;
}

void name_editor_move_vertical(name_editor_t *ed, int delta)
{
    if (!ed || !ed->active || delta == 0) return;

    int row = (int)ed->row + delta;
    const int count = 3;
    while (row < 0) row += count;
    while (row >= count) row -= count;
    ed->row = (uint8_t)row;

    int len = (int)strlen(row_text(ed, ed->row));
    if (len > 0 && ed->col >= len) ed->col = (uint8_t)(len - 1);
}

void name_editor_cursor_move(name_editor_t *ed, int delta)
{
    if (!ed || !ed->active || delta == 0) return;

    int len = (int)strnlen(ed->text, sizeof(ed->text) - 1);
    int cur = (int)ed->cursor + delta;
    if (cur < 0) cur = 0;
    if (cur > len) cur = len;
    ed->cursor = (uint8_t)cur;
}

void name_editor_toggle_case(name_editor_t *ed)
{
    if (ed && ed->active) ed->uppercase = !ed->uppercase;
}

void name_editor_backspace(name_editor_t *ed)
{
    if (!ed || !ed->active || ed->cursor == 0) return;

    size_t len = strnlen(ed->text, sizeof(ed->text) - 1);
    memmove(&ed->text[ed->cursor - 1],
            &ed->text[ed->cursor],
            len - ed->cursor + 1);
    --ed->cursor;
}

char name_editor_current_char(const name_editor_t *ed)
{
    if (!ed || !ed->active) return 0;
    const char *row = row_text(ed, ed->row);
    size_t len = strlen(row);
    return ed->col < len ? row[ed->col] : 0;
}

void name_editor_select(name_editor_t *ed)
{
    if (!ed || !ed->active) return;

    char ch = name_editor_current_char(ed);
    if (!ch) return;

    size_t len = strnlen(ed->text, sizeof(ed->text) - 1);
    if (len >= sizeof(ed->text) - 1) return;

    if (ed->cursor > len) ed->cursor = (uint8_t)len;

    memmove(&ed->text[ed->cursor + 1],
            &ed->text[ed->cursor],
            len - ed->cursor + 1);
    ed->text[ed->cursor] = ch;
    ++ed->cursor;
}

const char *name_editor_row_text(const name_editor_t *ed, uint8_t row)
{
    return row_text(ed, row);
}

uint8_t name_editor_row_count(void)
{
    return 3;
}

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool active;
    bool uppercase;
    uint8_t row;
    uint8_t col;
    uint8_t cursor;
    char text[CTRL_NAME_LEN];
} name_editor_t;

void name_editor_open(name_editor_t *ed, const char *initial);
void name_editor_close(name_editor_t *ed);

void name_editor_move_horizontal(name_editor_t *ed, int delta);
void name_editor_move_vertical(name_editor_t *ed, int delta);
void name_editor_cursor_move(name_editor_t *ed, int delta);
void name_editor_toggle_case(name_editor_t *ed);
void name_editor_backspace(name_editor_t *ed);
void name_editor_select(name_editor_t *ed);

const char *name_editor_row_text(const name_editor_t *ed, uint8_t row);
uint8_t name_editor_row_count(void);
char name_editor_current_char(const name_editor_t *ed);

#ifdef __cplusplus
}
#endif

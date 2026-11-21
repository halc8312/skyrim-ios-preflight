/* SPDX-License-Identifier: GPL-3.0-only */
#include "mod_ece_slot.h"
#include <limits.h>
#include <stdbool.h>
#include <string.h>

enum { NOSE_HEIGHT_ID = 12, SLOT_LINE_LIMIT = 1024, NUMBER_LIMIT = 64 };

typedef struct {
    const char *next, *end;
} SlotText;

static void skip_space(SlotText *text) {
    while (text->next < text->end && (*text->next == ' ' || *text->next == '\t')) ++text->next;
}

static bool same_text(SlotText text, const char *literal) {
    const size_t length = (size_t)(text.end - text.next);
    return strlen(literal) == length && !memcmp(text.next, literal, length);
}

static bool digit(SlotText text) {
    return text.next < text.end && *text.next >= '0' && *text.next <= '9';
}

static bool read_row_id(SlotText *line, unsigned *id, bool *negative) {
    *id = 0;
    *negative = line->next < line->end && *line->next == '-';
    if (*negative || (line->next < line->end && *line->next == '+')) ++line->next;
    if (!digit(*line)) return false;
    do {
        unsigned unit = (unsigned)(*line->next++ - '0');
        if (*id > (UINT_MAX - unit) / 10) return false;
        *id = *id * 10 + unit;
    } while (digit(*line));
    skip_space(line);
    if (line->next == line->end || *line->next != ',') return false;
    ++line->next;
    skip_space(line);
    return true;
}

/* The bundled presets use ASCII fixed decimals. Reading that grammar
 * directly keeps the input independent of the host's decimal separator. */
static bool read_value(SlotText line, double *value) {
    if ((size_t)(line.end - line.next) > NUMBER_LIMIT) return false;
    const bool negative = line.next < line.end && *line.next == '-';
    if (negative || (line.next < line.end && *line.next == '+')) ++line.next;
    if (!digit(line)) return false;
    double amount = 0;
    do { amount = amount * 10 + (*line.next++ - '0'); } while (digit(line));
    if (line.next < line.end && *line.next == '.') {
        ++line.next;
        if (!digit(line)) return false;
        double place = 0.1;
        do {
            amount += (*line.next++ - '0') * place;
            place *= 0.1;
        } while (digit(line));
    }
    if (line.next != line.end) return false;
    *value = negative ? -amount : amount;
    return true;
}

SkyrimEceSlotStatus skyrim_ece_slot_read_nose_height(const char *slot, size_t length, float *out) {
    if (length > SKYRIM_ECE_SLOT_LIMIT) return SKYRIM_ECE_SLOT_TOO_LARGE;
    if (!slot || !length || !out || memchr(slot, '\0', length)) return SKYRIM_ECE_SLOT_MALFORMED;

    const char *end = slot + length;
    if (length >= 3 && !memcmp(slot, "\xef\xbb\xbf", 3)) slot += 3;
    bool header = false, pairs = false, found = false;
    double nose_height = 0;
    while (slot < end) {
        const char *newline = memchr(slot, '\n', (size_t)(end - slot));
        const char *line_end = newline ? newline : end;
        if ((size_t)(line_end - slot) > SLOT_LINE_LIMIT) return SKYRIM_ECE_SLOT_MALFORMED;
        SlotText line = {slot, line_end};
        slot = newline ? newline + 1 : end;
        skip_space(&line);
        while (line.next < line.end &&
               (line.end[-1] == '\r' || line.end[-1] == ' ' || line.end[-1] == '\t')) --line.end;
        if (line.next == line.end) continue;
        if (!header) {
            if (!same_text(line, "# CharacterMakingExtender face data")) return SKYRIM_ECE_SLOT_MALFORMED;
            header = true;
            continue;
        }
        if (*line.next == '#') {
            if (same_text(line, "# 'uniqueID, value' pair")) {
                if (pairs) return SKYRIM_ECE_SLOT_MALFORMED;
                pairs = true;
            }
            continue;
        }
        unsigned id;
        bool negative;
        if (!pairs || !read_row_id(&line, &id, &negative)) return SKYRIM_ECE_SLOT_MALFORMED;
        const bool nose = !negative && id == NOSE_HEIGHT_ID;
        double value;
        if (!read_value(line, &value))
            return nose ? SKYRIM_ECE_SLOT_BAD_NOSE_HEIGHT : SKYRIM_ECE_SLOT_MALFORMED;
        if (nose) {
            if (found || value < -1e6 || value > 1e6) return SKYRIM_ECE_SLOT_BAD_NOSE_HEIGHT;
            nose_height = value;
            found = true;
        }
    }
    if (!pairs) return SKYRIM_ECE_SLOT_MALFORMED;
    if (!found) return SKYRIM_ECE_SLOT_NO_NOSE_HEIGHT;
    *out = (float)nose_height;
    return SKYRIM_ECE_SLOT_OK;
}

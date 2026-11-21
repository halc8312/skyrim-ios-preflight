/* SPDX-License-Identifier: GPL-3.0-only */
#include "form_view.h"

bool classic_read_form_id(const struct CPU *cpu, uint32_t address, uint8_t type,
                          uint32_t *form_id) {
    /* TESForm position of the ID and type, per SKSE 1.7.3 GameForms.h. */
    enum { ID_OFFSET = 0x0c, TYPE_OFFSET = 0x12 };
    uint8_t fields[TYPE_OFFSET - ID_OFFSET + 1];
    if (!address || (address & 3u) || address > UINT32_MAX - TYPE_OFFSET ||
        !classic_engine_read_data(cpu, address + ID_OFFSET, fields, sizeof(fields)) ||
        fields[TYPE_OFFSET - ID_OFFSET] != type) {
        return false;
    }
    *form_id = classic_read_le32(fields);
    return *form_id != 0;
}

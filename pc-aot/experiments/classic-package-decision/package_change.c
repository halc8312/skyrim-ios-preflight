/* SPDX-License-Identifier: GPL-3.0-only */
#include "package_change.h"
#include "form_view.h"

void classic_package_change_before_assign(const struct CPU *cpu, uint32_t event_address) {
    if (!classic_package_change_subscribed()) return;

    uint8_t fields[8];
    if (!event_address || event_address > UINT32_MAX - (sizeof(fields) - 1) ||
        !classic_engine_read_data(cpu, event_address, fields, sizeof(fields))) {
        return;
    }

    /* The LE engine consumes a live reference here. It looks up only the
     * second event word as a FormID. */
    const uint32_t actor = classic_read_le32(fields);
    const uint32_t package_id = classic_read_le32(fields + 4);
    uint32_t actor_id;
    if (!package_id || !classic_read_form_id(cpu, actor, CLASSIC_CHARACTER_FORM, &actor_id) ||
        classic_engine_lookup_form(cpu, actor_id) != actor) {
        return;
    }

    uint32_t checked_id;
    const uint32_t package = classic_engine_lookup_form(cpu, package_id);
    if (classic_read_form_id(cpu, package, CLASSIC_PACKAGE_FORM, &checked_id) &&
        checked_id == package_id) {
        classic_package_change_notify((ClassicPackageChange){actor_id, package_id});
    }
}

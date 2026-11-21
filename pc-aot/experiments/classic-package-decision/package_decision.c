/* SPDX-License-Identifier: GPL-3.0-only */
#include "package_decision.h"
#include "form_view.h"

uint32_t classic_package_decision_after_call(const struct CPU *cpu, uint32_t actor,
                                             uint32_t original_package) {
    if (!classic_package_decision_subscribed()) return original_package;

    ClassicPackageDecision decision = {0};
    if (!classic_read_form_id(cpu, actor, CLASSIC_CHARACTER_FORM, &decision.actor_id) ||
        (original_package &&
         !classic_read_form_id(cpu, original_package, CLASSIC_PACKAGE_FORM,
                               &decision.original_package_id))) {
        return original_package;
    }

    ClassicPackageReplacement replacement = classic_package_decision_choose(decision);
    if (!replacement.present || !replacement.package_id) return original_package;

    uint32_t address = classic_engine_lookup_form(cpu, replacement.package_id);
    uint32_t checked_id;
    if (!classic_read_form_id(cpu, address, CLASSIC_PACKAGE_FORM, &checked_id) ||
        checked_id != replacement.package_id) {
        return original_package;
    }
    return address;
}

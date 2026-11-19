/* SPDX-License-Identifier: GPL-3.0-only */
#include "menu_history.h"

typedef struct { SkseMenuObserver observer; void *context; } MenuObserver;
static void menu_row(void *arg, SkseRegistrationKey event, SkseRegistrationName callback,
                     uint64_t saved, SkseModResolution resolution, const uint64_t *mapped) {
    (void)callback;
    MenuObserver *menu = arg;
    menu->observer(menu->context, event.name.data, event.name.size, saved, resolution, mapped);
}

SkseMenuResult skse_menu_history_load(SksePairStore *store, const SksePairGeneration *generation,
                                     const SkseLoadedMod *current, size_t count,
                                     SkseMenuSummary *out, SkseMenuObserver observer, void *context,
                                     SksePairResult *source_result, SkseModMapResult *map_result) {
    MenuObserver menu = {observer, context};
    return (SkseMenuResult)skse_registration_history_load(store, generation, current, count, SKSE_REG_MENU_EVENTS,
        out, observer ? menu_row : NULL, &menu, source_result, map_result);
}

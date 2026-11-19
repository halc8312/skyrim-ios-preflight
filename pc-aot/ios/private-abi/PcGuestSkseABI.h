/* SPDX-License-Identifier: GPL-3.0-only
 * Optional interface between the two statically compiled halves of the Classic
 * experiment. The ordinary title host and guest keep their v1 interface.
 */
#pragma once
#include "PcGuestABI.h"

#define PC_GUEST_SKSE_ABI 1u
#define PC_GUEST_SKSE_API_SYMBOL "skyrim_guest_classic_api_v1"
#define PC_GUEST_SKSE_BIND_SYMBOL "skyrim_guest_bind_classic_v1"

typedef struct {
    uint32_t version, bytes;
    void (*register_papyrus)(CPU *);
    void (*create_menu)(CPU *);
    void (*inventory_entry)(CPU *);
    void (*inventory_form)(CPU *);
} PcGuestSkseCallbacks;

typedef const PcGuestSkseCallbacks *(*PcGuestBindClassic)(const PcGuestSkseCallbacks *);

static inline int pc_guest_skse_callbacks_valid(const PcGuestSkseCallbacks *table) {
    return table && table->version == PC_GUEST_SKSE_ABI && table->bytes == sizeof(*table)
        && table->register_papyrus && table->create_menu
        && table->inventory_entry && table->inventory_form;
}

int pc_guest_bind_classic_skse(void *library); /* host */
int pc_guest_skse_is_bound(void);             /* guest */

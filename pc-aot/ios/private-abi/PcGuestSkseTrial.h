/* Host-only launch choice. The signed guest's Classic v1 interface is unchanged. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Choose 0 or 1 on the startup thread after binding and before starting the guest.
 * The first successful call fixes the choice. Until then the original entries run. */
int pc_guest_select_classic_skse(int enabled);
extern int pc_guest_classic_skse_active;  /* read-only to callers, fixed during play */

#ifdef __cplusplus
}
#endif

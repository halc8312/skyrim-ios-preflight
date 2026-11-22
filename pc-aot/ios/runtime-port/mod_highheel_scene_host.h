/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "PcGuestModHooksABI.h"

void skyrim_highheel_scene_fill(PcGuestModHooks *);
#ifdef SKYRIM_IOS_CLASSIC_PAPYRUSUTIL
bool skyrim_highheel_scene_prepare_player_trial(void);
void skyrim_highheel_scene_cancel_player_trial(void);
#endif

/* SPDX-License-Identifier: GPL-3.0-only
 * Fail compilation if a desktop/simulator or fixed-override configuration leaks
 * into the original-guest device diagnostic archive.
 */
#pragma once
#if !defined(__APPLE__)
#error "SkyrimIOS runtime requires an Apple iOS device compiler"
#else
#include <TargetConditionals.h>
#if !TARGET_OS_IOS || TARGET_OS_SIMULATOR
#error "SkyrimIOS runtime requires an iPhone/iPad device SDK, not a simulator"
#endif
#endif
#if !defined(__aarch64__) && !defined(__arm64__)
#error "SkyrimIOS runtime requires ARM64"
#endif
#if !defined(GAME_SKYRIM) || defined(GAME_MARVEL) || defined(GAME_BATMAN)
#error "SkyrimIOS runtime requires the Skyrim 2011 guest configuration"
#endif
#if !defined(SKYRIM_IOS_GUEST_DIAGNOSTIC) || SKYRIM_IOS_GUEST_DIAGNOSTIC != 1
#error "This runtime archive requires original-guest diagnostic mode"
#endif
#if defined(SKYDREAM_SKYUI) || defined(SKYDREAM_EMBEDDED_IMAGE)
#error "SkyUI and embedded-image builds are not supported by this diagnostic archive"
#endif
#if defined(SKYRIM_IOS_CLASSIC_SKSE)
#if SKYRIM_IOS_CLASSIC_SKSE != 1 || !defined(SKYDREAM_SKSE) || SKYDREAM_SKSE != 1
#error "The classic Papyrus experiment requires the matching SKSE runtime"
#endif
#elif defined(SKYDREAM_SKSE)
#error "The ordinary iOS diagnostic archive does not include SKSE"
#endif
#if defined(SKYRIM_IOS_MOD_HOOKS) && (SKYRIM_IOS_MOD_HOOKS != 1 || !defined(SKYRIM_IOS_CLASSIC_SKSE))
#error "The guest MOD hook interface requires the Classic Papyrus experiment"
#endif

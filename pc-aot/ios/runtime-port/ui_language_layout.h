/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#import <UIKit/UIKit.h>
#import "ui_language.h"

/* Change only the native menus. Directional pads and the gameplay viewport
 * represent physical directions regardless of the menu's writing direction. */
static inline void SKApplyMenuLanguage(UINavigationController *nav) {
    SKUILanguage language = SKCurrentUILanguage();
    UISemanticContentAttribute direction = language == SKUIArabic
        ? UISemanticContentAttributeForceRightToLeft : UISemanticContentAttributeForceLeftToRight;
    nav.view.semanticContentAttribute = direction;
    nav.view.accessibilityLanguage = SKUILanguageCode(language);
    nav.navigationBar.semanticContentAttribute = direction;
    nav.navigationBar.accessibilityLanguage = nav.view.accessibilityLanguage;
    nav.topViewController.view.semanticContentAttribute = direction;
    [nav.view setNeedsLayout];
}

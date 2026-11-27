/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#import <Foundation/Foundation.h>

/* Stored in mobile-settings-v1. These values are independent of game text/voices. */
typedef NS_ENUM(NSInteger, SKUILanguage) {
    SKUIEnglish, SKUIJapanese, SKUIArabic, SKUISpanish, SKUIFrench
};

NSArray<NSNumber *> *SKUILanguageChoices(void);
NSArray<NSString *> *SKUILanguageNames(void);
NSString *SKUILanguageCode(SKUILanguage language);
SKUILanguage SKUILanguageFromPreference(id preference);
SKUILanguage SKPreferredUILanguage(NSArray<NSString *> *preferredLanguages);
SKUILanguage SKCurrentUILanguage(void);
void SKRefreshUILanguage(void);
NSString *SKUIString(SKUILanguage language, NSString *english, NSString *japanese);

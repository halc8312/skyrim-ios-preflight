/* SPDX-License-Identifier: GPL-3.0-only */
#import "ui_language.h"
#include <math.h>
#include <stdatomic.h>

NSArray<NSNumber *> *SKUILanguageChoices(void) {
    return @[@(SKUIEnglish), @(SKUIJapanese), @(SKUIArabic), @(SKUISpanish), @(SKUIFrench)];
}
NSArray<NSString *> *SKUILanguageNames(void) {
    return @[@"English", @"日本語", @"العربية", @"Español", @"Français"];
}
NSString *SKUILanguageCode(SKUILanguage language) {
    switch (language) {
        case SKUIJapanese: return @"ja";
        case SKUIArabic: return @"ar";
        case SKUISpanish: return @"es";
        case SKUIFrench: return @"fr";
        default: return @"en";
    }
}
SKUILanguage SKUILanguageFromPreference(id preference) {
    if (![preference isKindOfClass:NSNumber.class]) return SKUIEnglish;
    double code = [preference doubleValue];
    return code >= SKUIEnglish && code <= SKUIFrench && code == trunc(code) ? (SKUILanguage)code : SKUIEnglish;
}
SKUILanguage SKPreferredUILanguage(NSArray<NSString *> *preferredLanguages) {
    for (NSString *identifier in preferredLanguages) {
        if (![identifier isKindOfClass:NSString.class]) continue;
        NSString *code = [NSLocale componentsFromLocaleIdentifier:identifier][NSLocaleLanguageCode];
        for (NSNumber *choice in SKUILanguageChoices()) {
            if ([code.lowercaseString isEqualToString:SKUILanguageCode(choice.integerValue)])
                return (SKUILanguage)choice.integerValue;
        }
    }
    return SKUIEnglish;
}

/* Import workers also create notices. Publish the numeric selection without
 * consulting defaults on the gameplay tick or for each translated table row. */
static atomic_int activeLanguage;
void SKRefreshUILanguage(void) {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    @synchronized (defaults) {
        id settings = [defaults objectForKey:@"mobile-settings-v1"];
        SKUILanguage selected = SKUIEnglish;
        if (!settings) selected = SKPreferredUILanguage(NSLocale.preferredLanguages);
        else if ([settings isKindOfClass:NSDictionary.class])
            selected = SKUILanguageFromPreference(settings[@"language"]);
        atomic_store(&activeLanguage, (int)selected);
    }
}
SKUILanguage SKCurrentUILanguage(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        [NSNotificationCenter.defaultCenter addObserverForName:NSUserDefaultsDidChangeNotification
            object:NSUserDefaults.standardUserDefaults queue:nil usingBlock:^(NSNotification *note) {
                (void)note;
                SKRefreshUILanguage();
            }];
        SKRefreshUILanguage();
    });
    return (SKUILanguage)atomic_load(&activeLanguage);
}
NSString *SKUIString(SKUILanguage language, NSString *english, NSString *japanese) {
    if (language == SKUIJapanese) return japanese ?: english;
    if (language < SKUIArabic || language > SKUIFrench) return english;
    /* Pick the requested translation even when the device is in another language. */
    static NSDictionary<NSNumber *, NSBundle *> *translations;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSMutableDictionary *bundles = [NSMutableDictionary dictionary];
        for (NSNumber *choice in @[@(SKUIArabic), @(SKUISpanish), @(SKUIFrench)]) {
            NSURL *url = [NSBundle.mainBundle URLForResource:SKUILanguageCode(choice.integerValue)
                                               withExtension:@"lproj"];
            NSBundle *bundle = url ? [NSBundle bundleWithURL:url] : nil;
            if (bundle) bundles[choice] = bundle;
        }
        translations = [bundles copy];
    });
    return [translations[@(language)] localizedStringForKey:english value:english table:@"AppUI"] ?: english;
}

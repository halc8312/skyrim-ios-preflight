/* SPDX-License-Identifier: GPL-3.0-only */
#import <XCTest/XCTest.h>
#import <CoreGraphics/CoreGraphics.h>
#include <math.h>

// A virtualized cell can still exist in the tree with {{inf,inf},{0,0}}
// during dismissal. XCTest records a test failure if asked to hit-test it.
static CGRect elementFrame(XCUIElement *element) {
    if (!element.exists) return CGRectNull;
    CGRect frame = element.frame;
    if (CGRectIsEmpty(frame) || CGRectIsInfinite(frame) ||
        !isfinite(frame.origin.x) || !isfinite(frame.origin.y) ||
        !isfinite(frame.size.width) || !isfinite(frame.size.height)) return CGRectNull;
    return frame;
}

static BOOL canHit(XCUIElement *element) {
    return !CGRectIsNull(elementFrame(element)) && element.hittable;
}

@interface ModScreensTests : XCTestCase
@property(nonatomic,strong) XCUIApplication *app;
@end

typedef NS_ENUM(NSUInteger, SKPickerLocation) {
    SKPickerInitial, SKPickerBrowse, SKPickerLocal, SKPickerAppFolder, SKPickerSamples
};

@implementation ModScreensTests
- (void)setUp {
    [super setUp];
    self.continueAfterFailure = NO;
    self.app = [XCUIApplication new];
    self.app.launchArguments = @[@"-AppleLanguages", @"(en)", @"-AppleLocale", @"en_US"];
}
- (void)capture:(NSString *)name {
    XCTAttachment *attachment = [XCTAttachment attachmentWithScreenshot:XCUIScreen.mainScreen.screenshot];
    attachment.lifetime = XCTAttachmentLifetimeKeepAlways;
    attachment.name = name;
    [self addAttachment:attachment];
}
- (void)tearDown {
    if (self.testRun.totalFailureCount) [self capture:@"failure-screen"];
    [super tearDown];
}

- (XCUIElement *)findRow:(NSString *)label {
    // A presented navigation controller leaves the hub's table in the
    // accessibility tree. First match the row's actual table, then z-order.
    XCUIElementQuery *tables = [self.app.tables containingType:XCUIElementTypeStaticText identifier:label];
    XCUIElement *table = nil, *row = nil;
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:15];
    do {
        for (XCUIElement *candidate in tables.allElementsBoundByIndex.reverseObjectEnumerator) {
            XCUIElement *cell = [candidate.cells containingType:XCUIElementTypeStaticText identifier:label].firstMatch;
            CGRect cellFrame = elementFrame(cell), tableFrame = elementFrame(candidate);
            if (CGRectIsNull(tableFrame) || CGRectIsNull(cellFrame)) continue;
            if (candidate.hittable || (CGRectIntersectsRect(cellFrame, tableFrame) && canHit(cell))) {
                table = candidate; row = cell; break;
            }
        }
        if (table) break;
        [NSThread sleepForTimeInterval:.15];
    } while (until.timeIntervalSinceNow > 0);
    XCTAssertNotNil(table, @"Missing foreground table with row %@. %@", label, self.app.debugDescription);
    // UIKit reports real offscreen cell frames even when all their child labels
    // have the same placeholder Y. Use the cell for scrolling and the final tap.
    XCUIElement *nav = nil;
    for (XCUIElement *candidate in self.app.navigationBars.allElementsBoundByIndex.reverseObjectEnumerator)
        if (canHit(candidate)) { nav = candidate; break; }
    CGFloat previousY = CGFLOAT_MAX;
    for (NSUInteger i = 0; i < 40; ++i) {
        CGRect bounds = elementFrame(table), cellFrame = elementFrame(row);
        if (CGRectIsNull(bounds) || CGRectIsNull(cellFrame)) {
            [NSThread sleepForTimeInterval:.15];
            continue;
        }
        CGFloat top = CGRectGetMinY(bounds) + 10;
        CGRect navFrame = elementFrame(nav);
        if (!CGRectIsNull(navFrame)) top = MAX(top, CGRectGetMaxY(navFrame) + 8);
        CGFloat bottom = CGRectGetMaxY(bounds) - 22;
        CGFloat y = CGRectGetMidY(cellFrame);
        if (y >= top && y <= bottom) {
            if (canHit(row)) return row;
            [NSThread sleepForTimeInterval:.15];
            continue;
        }
        if (bottom <= top || ABS(y - previousY) < 1) break;
        previousY = y;
        CGFloat middle = (top + bottom) / 2;
        CGFloat limit = (bottom - top) * .65;
        CGFloat drag = MIN(MAX(y - middle, -limit), limit);
        CGFloat from = (middle + drag / 2 - CGRectGetMinY(bounds)) / CGRectGetHeight(bounds);
        CGFloat to = (middle - drag / 2 - CGRectGetMinY(bounds)) / CGRectGetHeight(bounds);
        XCUICoordinate *start = [table coordinateWithNormalizedOffset:CGVectorMake(.5, from)];
        XCUICoordinate *end = [table coordinateWithNormalizedOffset:CGVectorMake(.5, to)];
        [start pressForDuration:.08 thenDragToCoordinate:end withVelocity:420 thenHoldForDuration:.2];
    }
    XCTFail(@"Could not bring the row %@ onscreen. %@", label, table.debugDescription);
    return row;
}
- (void)goBackFrom:(NSString *)title {
    XCUIElement *bar = self.app.navigationBars[title];
    XCTAssertTrue([bar waitForExistenceWithTimeout:8], @"Missing navigation bar %@", title);
    XCUIElement *nativeBack = bar.buttons[@"BackButton"];
    if (nativeBack.exists) { [nativeBack tap]; return; }
    CGRect barFrame = elementFrame(bar);
    XCTAssertFalse(CGRectIsNull(barFrame), @"Invalid navigation bar on %@", title);
    XCUIElement *back = nil;
    CGFloat leading = CGRectGetMidX(barFrame);
    for (XCUIElement *button in bar.buttons.allElementsBoundByIndex) {
        CGRect frame = elementFrame(button);
        if (CGRectIsNull(frame)) continue;
        CGFloat x = CGRectGetMidX(frame);
        if (x < leading && button.hittable) { back = button; leading = x; }
    }
    XCTAssertNotNil(back, @"Missing leading back button on %@. %@", title, bar.debugDescription);
    [back tap];
}
- (void)openHub {
    XCUIElement *settings = self.app.navigationBars[@"SkyrimIOS Settings"];
    XCTAssertTrue([settings waitForExistenceWithTimeout:20]);
    [[self findRow:@"Manage MODs"] tap];
    XCTAssertTrue([self.app.navigationBars[@"MOD"] waitForExistenceWithTimeout:8]);
}
- (void)fillName:(NSString *)name replacing:(NSString *)initial inAlert:(NSString *)title {
    XCUIElement *alert = self.app.alerts[title];
    XCUIElement *field = alert.textFields.firstMatch;
    XCTAssertTrue([field waitForExistenceWithTimeout:6]);
    XCTAssertEqualObjects(field.value, initial);
    [[field coordinateWithNormalizedOffset:CGVectorMake(0.8, 0.5)] tap];
    if ([name hasPrefix:initial]) {
        [field typeText:[name substringFromIndex:initial.length]];
    } else {
        NSMutableString *keys = [NSMutableString string];
        for (NSUInteger i = 0; i < initial.length; ++i) [keys appendString:XCUIKeyboardKeyDelete];
        [keys appendString:name];
        [field typeText:keys];
    }
    XCTAssertEqualObjects(field.value,name);
    [alert.buttons[@"OK"] tap];
}

// The "File View" collection contains document cells; Files has separate
// sidebar and tab collections which can stay visible even deep inside a folder.
- (XCUIElement *)fileNamed:(NSString *)name inCollection:(XCUIElement *)files {
    NSString *shortName = name.stringByDeletingPathExtension;
    NSPredicate *matches = [NSPredicate predicateWithFormat:
        @"label IN[c] %@ OR identifier IN[c] %@ OR label BEGINSWITH[c] %@ OR label BEGINSWITH[c] %@ "
         "OR identifier BEGINSWITH[c] %@ OR identifier BEGINSWITH[c] %@",
        @[name, shortName], @[name, shortName], [name stringByAppendingString:@","], [shortName stringByAppendingString:@","],
        [name stringByAppendingString:@","], [shortName stringByAppendingString:@","]];
    XCUIElement *cell = [files.cells matchingPredicate:matches].firstMatch;
    if (cell.exists) return cell;
    // Some Files layouts present the display name only in a child title.
    XCUIElement *displayName = [files.cells containingType:XCUIElementTypeStaticText identifier:shortName].firstMatch;
    if ([shortName isEqualToString:name] || displayName.exists) return displayName;
    return [files.cells containingType:XCUIElementTypeStaticText identifier:name].firstMatch;
}
- (void)scrollFiles:(XCUIElement *)files towardBottom:(BOOL)down {
    CGRect bounds = elementFrame(files);
    if (CGRectIsNull(bounds)) return;
    CGFloat left = CGRectGetMinX(bounds) + 8, right = CGRectGetMaxX(bounds) - 8;
    CGFloat top = CGRectGetMinY(bounds) + 8, bottom = CGRectGetMaxY(bounds) - 12;
    XCUIElement *sidebar = self.app.collectionViews[@"Browse View"];
    XCUIElement *bar = self.app.navigationBars[@"FullDocumentManagerViewControllerNavigationBar"];
    XCUIElement *tabs = self.app.tabBars[@"DOC.browsingModeTabBar"];
    CGRect sideFrame = elementFrame(sidebar), barFrame = elementFrame(bar), tabFrame = elementFrame(tabs);
    if (!CGRectIsNull(sideFrame) && sidebar.hittable) left = MAX(left, CGRectGetMaxX(sideFrame) + 8);
    if (!CGRectIsNull(barFrame)) top = MAX(top, CGRectGetMaxY(barFrame) + 8);
    if (!CGRectIsNull(tabFrame)) bottom = MIN(bottom, CGRectGetMinY(tabFrame) - 8);
    if (right <= left || bottom - top < 45) return;
    CGFloat x = ((left + right) / 2 - CGRectGetMinX(bounds)) / CGRectGetWidth(bounds);
    CGFloat high = (top + .15 * (bottom - top) - CGRectGetMinY(bounds)) / CGRectGetHeight(bounds);
    CGFloat low = (top + .85 * (bottom - top) - CGRectGetMinY(bounds)) / CGRectGetHeight(bounds);
    XCUICoordinate *from = [files coordinateWithNormalizedOffset:CGVectorMake(x, down ? low : high)];
    XCUICoordinate *to = [files coordinateWithNormalizedOffset:CGVectorMake(x, down ? high : low)];
    [from pressForDuration:.08 thenDragToCoordinate:to withVelocity:300 thenHoldForDuration:.2];
}
- (XCUIElement *)locateFile:(NSString *)name {
    XCUIElement *files = self.app.collectionViews[@"File View"];
    XCUIElement *bar = self.app.navigationBars[@"FullDocumentManagerViewControllerNavigationBar"];
    XCUIElement *local = self.app.cells[@"DOC.sidebar.item.On My iPhone"];
    XCUIElement *browse = self.app.tabBars.buttons[@"Browse"];
    SKPickerLocation stage = SKPickerInitial;
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:35];
    NSDate *ready = [NSDate dateWithTimeIntervalSinceNow:3];
    NSUInteger scrolls = 0;
    while (deadline.timeIntervalSinceNow > 0) {
        XCUIElement *item = [self fileNamed:name inCollection:files];
        if (canHit(item) && item.enabled) return item;
        if (stage < SKPickerSamples && bar.staticTexts[@"UI Samples"].exists) {
            stage = SKPickerSamples;
            ready = [NSDate dateWithTimeIntervalSinceNow:3];
            deadline = [NSDate dateWithTimeIntervalSinceNow:22];
        }
        if (ready.timeIntervalSinceNow <= 0) {
            if (stage == SKPickerSamples) {
                // This state is terminal: a still-visible Browse button leads
                // back to the root. Wait for File Provider to populate its list.
                if (scrolls < 8 && files.exists && files.cells.count) {
                    CGRect frame = elementFrame(item);
                    BOOL down = CGRectIsNull(frame) ? scrolls < 4 : CGRectGetMidY(frame) >= CGRectGetMidY(files.frame);
                    [self scrollFiles:files towardBottom:down];
                    ++scrolls;
                    ready = [NSDate dateWithTimeIntervalSinceNow:.4];
                }
            } else {
                XCUIElement *samples = [self fileNamed:@"UI Samples" inCollection:files];
                XCUIElement *place = nil;
                if (canHit(samples)) { place = samples; stage = SKPickerSamples; }
                if (!place && stage < SKPickerAppFolder) {
                    XCUIElement *host = [self fileNamed:@"MOD Screen Test" inCollection:files];
                    if (canHit(host)) { place = host; stage = SKPickerAppFolder; }
                }
                if (!place && stage < SKPickerLocal && canHit(local)) {
                    place = local; stage = SKPickerLocal;
                }
                if (!place && stage < SKPickerBrowse && canHit(browse)) {
                    place = browse; stage = SKPickerBrowse;
                }
                if (place) {
                    [place tap];
                    ready = [NSDate dateWithTimeIntervalSinceNow:stage == SKPickerSamples ? 3 : .4];
                    deadline = [NSDate dateWithTimeIntervalSinceNow:22];
                }
            }
        }
        [NSThread sleepForTimeInterval:.15];
    }
    XCTFail(@"Files could not locate %@ at stage %lu. %@", name, (unsigned long)stage, self.app.debugDescription);
    return nil;
}
- (void)pick:(NSString *)name folder:(BOOL)folder {
    XCUIElement *item = [self locateFile:name];
    [self capture:[@"picker-" stringByAppendingString:name]];
    [item tap];
    // Folders must be opened then chosen; multi-select archives may require
    // an explicit Open. A single-select JSON picker dismisses on selection.
    if (folder || [name.pathExtension.lowercaseString isEqualToString:@"zip"]) {
        NSPredicate *confirmLabels = [NSPredicate predicateWithFormat:@"label IN %@", @[@"Open", @"Choose"]];
        XCUIElementQuery *choices = [self.app.buttons matchingPredicate:confirmLabels];
        NSDate *until = [NSDate dateWithTimeIntervalSinceNow:6];
        do {
            for (XCUIElement *confirm in choices.allElementsBoundByIndex)
                if (canHit(confirm) && confirm.enabled) { [confirm tap]; return; }
            if (!self.app.collectionViews[@"File View"].exists &&
                !self.app.navigationBars[@"FullDocumentManagerViewControllerNavigationBar"].exists) return;
            [NSThread sleepForTimeInterval:.15];
        } while (until.timeIntervalSinceNow > 0);
    } else {
        XCUIElement *files = self.app.collectionViews[@"File View"];
        XCUIElement *picker = self.app.navigationBars[@"FullDocumentManagerViewControllerNavigationBar"];
        XCUIElement *hub = self.app.navigationBars[@"MOD"];
        NSDate *until = [NSDate dateWithTimeIntervalSinceNow:18];
        do {
            if (!files.exists && !picker.exists && canHit(hub)) return;
            [NSThread sleepForTimeInterval:.15];
        } while (until.timeIntervalSinceNow > 0);
        XCTFail(@"Files did not close and return to MOD after selecting %@.", name);
    }
}
- (void)recordNexusResult:(NSString *)result detail:(NSString *)detail {
    [self capture:[@"nexus-live-" stringByAppendingString:result]];
    NSString *text = [NSString stringWithFormat:@"result=%@\n%@\n", result, detail];
    XCTAttachment *report = [XCTAttachment attachmentWithData:[text dataUsingEncoding:NSUTF8StringEncoding]
                                       uniformTypeIdentifier:@"public.utf8-plain-text"];
    report.name = @"nexus-live-result.txt";
    report.lifetime = XCTAttachmentLifetimeKeepAlways;
    [self addAttachment:report];
}

// Run this network-dependent test on its own fresh simulator. Do not tap a
// link, dismiss a third-party interstitial, sign in, or request a download.
- (void)testLiveNexusListing {
    self.app.launchArguments = [self.app.launchArguments arrayByAddingObject:@"-ObserveNexusLiveNavigation"];
    [self.app launch];
    [self openHub];
    [[self findRow:@"Find on Nexus Mods"] tap];
    XCUIElement *bar = self.app.navigationBars[@"Nexus Mods"];
    XCUIElement *web = self.app.webViews.firstMatch;
    XCTAssertTrue([bar waitForExistenceWithTimeout:10]);
    XCTAssertTrue([web waitForExistenceWithTimeout:12]);

    NSPredicate *listing = [NSPredicate predicateWithFormat:@"label MATCHES[c] %@",
        @"(Skyrim mods|Clear game filter|Search parameters|Browse the internet['’]s best mods.*)"];
    NSPredicate *challenge = [NSPredicate predicateWithFormat:@"label MATCHES[c] %@",
        @".*(Just a moment|verif(y|ying) (that )?you are human|checking your browser|security verification|"
         "checking if the site connection is secure|review the security of your connection|attention required).*"];
    NSPredicate *siteError = [NSPredicate predicateWithFormat:@"label MATCHES[c] %@",
        @".*(Access denied|403 Forbidden|Bad gateway|Gateway time.?out|Service unavailable|"
         "This site can.t be reached|failed to connect to the server).*"];
    NSPredicate *interstitial = [NSPredicate predicateWithFormat:@"label MATCHES[c] %@",
        @".*(We value your privacy|Before you continue to Nexus Mods).*"];
    XCUIElementQuery *document = [web descendantsMatchingType:XCUIElementTypeAny];
    XCUIElementQuery *blockers = [document matchingPredicate:
        [NSCompoundPredicate orPredicateWithSubpredicates:@[challenge, siteError, interstitial]]];
    XCUIElementQuery *markers = [document matchingPredicate:listing];
    CGRect screen = elementFrame(self.app), nav = elementFrame(bar);
    XCTAssertFalse(CGRectIsNull(screen) || CGRectIsNull(nav), @"The browser has no finite onscreen viewport.");
    CGFloat top = MAX(CGRectGetMinY(screen), CGRectGetMaxY(nav));
    CGRect content = CGRectMake(CGRectGetMinX(screen), top, CGRectGetWidth(screen), CGRectGetMaxY(screen) - top);
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:60];
    NSString *result = @"unconfirmed", *detail = @"No visible listing marker.";
    NSUInteger scrolls = 0;
    do {
        BOOL officialURL = bar.buttons[@"nexus-live-listing-url"].exists;
        if (!officialURL && bar.buttons[@"nexus-live-network-error"].exists) {
            result = @"network-error"; detail = @"WebKit reported a top-level URL loading error."; break;
        }
        if (!officialURL && bar.buttons[@"nexus-live-navigation-error"].exists) {
            result = @"navigation-error"; detail = @"WebKit reported a top-level navigation error."; break;
        }
        NSString *marker = nil;
        NSString *blocker = [challenge evaluateWithObject:web] ? @"challenge" : nil;
        BOOL listingBelow = NO;
        CGRect viewport = CGRectIntersection(content, elementFrame(web));
        // Check overlays before looking for listing text. A genuine heading
        // may be AX Other with no activation point. Read its frame and label
        // from the same snapshot, and stop querying as soon as it is visible.
        for (XCUIElementQuery *group in @[blockers, markers]) {
            BOOL lookingForListing = group == markers;
            if (blocker || CGRectIsEmpty(viewport) || (lookingForListing && !officialURL)) break;
            for (XCUIElement *element in group.allElementsBoundByIndex) {
                NSError *error = nil;
                id<XCUIElementSnapshot> match = [element snapshotWithError:&error];
                if (!match) continue;
                if (lookingForListing && ![listing evaluateWithObject:match]) continue;
                XCUIElementType type = match.elementType;
                if (!lookingForListing && type == XCUIElementTypeWebView && [challenge evaluateWithObject:match]) {
                    blocker = @"challenge"; break;
                }
                if (type != XCUIElementTypeStaticText && type != XCUIElementTypeLink &&
                    type != XCUIElementTypeButton && type != XCUIElementTypeOther) continue;
                CGRect frame = match.frame;
                if (CGRectIsEmpty(frame) || CGRectIsInfinite(frame) || !isfinite(frame.origin.x) ||
                    !isfinite(frame.origin.y) || !isfinite(frame.size.width) || !isfinite(frame.size.height) ||
                    (lookingForListing && type == XCUIElementTypeOther &&
                     CGRectGetHeight(frame) > CGRectGetHeight(viewport) / 2)) continue;
                if (lookingForListing && CGRectGetMinY(frame) >= CGRectGetMaxY(viewport)) listingBelow = YES;
                CGRect visible = CGRectIntersection(viewport, frame);
                if (CGRectIsEmpty(visible) || CGRectGetWidth(visible) < 2 || CGRectGetHeight(visible) < 2) continue;
                if (lookingForListing) marker = match.label;
                else blocker = [challenge evaluateWithObject:match] ? @"challenge" :
                               [siteError evaluateWithObject:match] ? @"site-error" : @"interstitial";
                break;
            }
        }
        if (blocker) {
            result = blocker; detail = @"The website displayed an interstitial or error.";
        } else if (officialURL && marker) {
            [self recordNexusResult:@"listing" detail:[@"Visible on the committed Skyrim listing URL: " stringByAppendingString:marker]];
            return;
        } else {
            result = !officialURL && bar.buttons[@"nexus-live-other-url"].exists ? @"other-url" : @"unconfirmed";
            detail = officialURL ? @"The Skyrim listing URL committed, but no listing content was visible." :
                                   @"No visible listing on a committed Skyrim listing URL.";
            // Reveal text already present in the document when its landmark
            // is below the landscape viewport. Never scroll an interstitial.
            if (officialURL && listingBelow && scrolls < 2) { [web swipeUp]; ++scrolls; }
        }
        [NSThread sleepForTimeInterval:1];
    } while (deadline.timeIntervalSinceNow > 0);
    [self recordNexusResult:result detail:detail];
    XCTFail(@"Nexus live result %@: %@", result, detail);
}

- (void)testActualSettingsModScreens {
    [self.app launch];
    XCTAssertTrue([self.app.tables[@"mod-translation-bridge-checked"] waitForExistenceWithTimeout:10]);
    [self capture:@"settings"];
    [[self findRow:@"App language"] tap];
    [self.app.buttons[@"日本語"] tap];
    [[self findRow:@"MODの管理"] tap];
    XCTAssertTrue([self.app.staticTexts[@"MODはまだありません"] waitForExistenceWithTimeout:8]);
    [self findRow:@"セーブの確認"];
    [self capture:@"mod-hub-japanese"];
    [self goBackFrom:@"MOD"];
    [[self findRow:@"アプリの言語"] tap];
    [self.app.buttons[@"English"] tap];
    [self openHub];
    XCTAssertTrue(self.app.staticTexts[@"No MODs yet"].exists);
    [self capture:@"empty-hub"];
#ifdef SKYRIM_IOS_CLASSIC_SKSE
    [self findRow:@"SkyUI 5.1 trial"];
    XCUIElement *trialToggle = self.app.switches[@"mod-skyui-trial"];
    XCTAssertTrue([trialToggle waitForExistenceWithTimeout:5]);
    XCTAssertEqualObjects([trialToggle.value description], @"0");
    [trialToggle tap];
    XCTAssertEqualObjects([trialToggle.value description], @"1");
    [self goBackFrom:@"MOD"];
    [self openHub];
    [self findRow:@"SkyUI 5.1 trial"];
    XCTAssertEqualObjects([trialToggle.value description], @"1");
    [trialToggle tap];
    XCTAssertEqualObjects([trialToggle.value description], @"0");
#endif
    [[self findRow:@"Save check"] tap];
    [self findRow:@"Authored Save"];
    NSPredicate *missingPlugin = [NSPredicate predicateWithFormat:@"label CONTAINS %@", @"UnavailableFixture.esp"];
    XCTAssertTrue([self.app.staticTexts matchingPredicate:missingPlugin].count > 0);
    [self capture:@"save-check"]; [self goBackFrom:@"Save check"];

    [[self findRow:@"Add a zip file"] tap];
    [self pick:@"AuthoredZip.zip" folder:NO];
    [self findRow:@"AuthoredZip"];
    [self capture:@"zip-imported"];
    [[self findRow:@"Add a folder"] tap];
    [self pick:@"AuthoredFolder" folder:YES];
    [self findRow:@"AuthoredFolder"];
    [self capture:@"folder-imported"];
    [[self findRow:@"Add a zip file"] tap];
    [self pick:@"AuthoredInstaller.zip" folder:NO];
    // The package is named in the compact header or the regular navigation prompt.
    XCUIElement *installer = self.app.navigationBars[@"Step 1/1"];
    XCTAssertTrue([installer waitForExistenceWithTimeout:15]);
    XCUIElement *description = self.app.staticTexts[@"mod-installer-description"];
    XCTAssertTrue([description waitForExistenceWithTimeout:8]);
    XCTAssertTrue([description.label containsString:@"Choose a colour"]);
    XCTAssertTrue([description.label containsString:@"AuthoredInstaller"] || installer.staticTexts[@"AuthoredInstaller"].exists);
    [[self findRow:@"Red"] pressForDuration:.8];
    XCUIElement *details = self.app.navigationBars[@"Option details"];
    XCTAssertTrue([details waitForExistenceWithTimeout:8]);
    XCUIElement *text = [[[self.app descendantsMatchingType:XCUIElementTypeAny]
        matchingIdentifier:@"mod-option-full-text"] firstMatch];
    XCTAssertTrue([text waitForExistenceWithTimeout:8]);
    XCTAssertTrue([text.label containsString:@"Install the red sample file."] ||
        [[text.value description] containsString:@"Install the red sample file."]);
    XCTAssertTrue(details.buttons[@"Copy"].exists);
    [self capture:@"fomod-full-text"];
    [details.buttons[@"Done"] tap];
    [[self findRow:@"Red"] tap];
    [self capture:@"fomod-options"];
    [installer.buttons[@"Install"] tap];
    [self findRow:@"AuthoredInstaller"];

    [[self findRow:@"Default"] tap];
    [self.app.navigationBars[@"Profiles"].buttons[@"Add"] tap];
    [self fillName:@"Trial" replacing:@"MOD" inAlert:@"New profile"];
    XCUIElement *trial = [self findRow:@"Trial"];
    [trial swipeLeft];
    if (![self.app.alerts[@"Rename"] waitForExistenceWithTimeout:1.5]) {
        XCUIElement *rename = self.app.buttons[@"Rename"];
        XCTAssertTrue([rename waitForExistenceWithTimeout:6]);
        [rename tap];
    }
    [self fillName:@"Trial Two" replacing:@"Trial" inAlert:@"Rename"];
    [[self findRow:@"Trial Two"] tap]; [self capture:@"profile-renamed-selected"];
    [self goBackFrom:@"Profiles"];
    [[self findRow:@"Find on Nexus Mods"] tap];
    XCTAssertTrue([self.app.navigationBars[@"Nexus Mods"] waitForExistenceWithTimeout:10]);
    XCTAssertTrue([self.app.webViews.firstMatch waitForExistenceWithTimeout:12]);
    [self capture:@"nexus-browser"]; [self goBackFrom:@"Nexus Mods"];

    [[self findRow:@"Share this MOD list"] tap];
    // The activity sheet is genuine. The simulator host later copies this
    // exact exported file to its Files documents for deterministic re-import.
    NSPredicate *shareDestination = [NSPredicate predicateWithFormat:
        @"label IN[c] %@ OR identifier CONTAINS[c] %@", @[@"Copy", @"Save to Files"], @"SaveToFiles"];
    XCUIElement *systemAction = [[self.app descendantsMatchingType:XCUIElementTypeAny]
                                      matchingPredicate:shareDestination].firstMatch;
    XCTAssertTrue([systemAction waitForExistenceWithTimeout:12],
                  @"Expected an action in the standard share sheet. %@",self.app.debugDescription);
    [self capture:@"export-share-sheet"];

    [self.app terminate];
    self.app.launchArguments=[self.app.launchArguments arrayByAddingObject:@"-ReimportAuthoredExport"];
    [self.app launch]; [self openHub];
    [self findRow:@"Trial Two"];
    [[self findRow:@"Conflicts"] tap];
    XCUIElement *conflict=[self.app.staticTexts matchingPredicate:[NSPredicate predicateWithFormat:@"label CONTAINS %@ AND label CONTAINS %@",@"AuthoredFolder",@"AuthoredZip"]].firstMatch;
    XCTAssertTrue([conflict waitForExistenceWithTimeout:6]);
    XCUIElement *conflictRow = [self findRow:conflict.label];
    [self capture:@"conflict-summary"]; [conflictRow tap];
    XCUIElement *file=[self.app.staticTexts matchingPredicate:[NSPredicate predicateWithFormat:@"label ENDSWITH[c] %@",@"shared.txt"]].firstMatch;
    XCTAssertTrue(file.exists); [self findRow:file.label]; [self capture:@"conflict-files"];
    [self goBackFrom:@"Files"]; [self goBackFrom:@"Conflicts"];
    [[self findRow:@"Save check"] tap];
    [self findRow:@"No saves in this profile"];
    [self capture:@"separate-profile-saves"]; [self goBackFrom:@"Save check"];
    [[self findRow:@"Load a MOD list"] tap];
    [self pick:@"Trial Two.skyrimios-modlist.json" folder:NO];
    [[self findRow:@"Trial Two"] tap];
    [self findRow:@"Trial Two (imported)"];
    [self capture:@"imported-profile"];
}
@end

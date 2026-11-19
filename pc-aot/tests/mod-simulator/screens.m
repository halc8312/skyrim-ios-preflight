/* SPDX-License-Identifier: GPL-3.0-only */
#import <XCTest/XCTest.h>
#import <CoreGraphics/CoreGraphics.h>

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
            if (cell.hittable || candidate.hittable) { table = candidate; row = cell; break; }
        }
        if (table) break;
        [NSThread sleepForTimeInterval:.15];
    } while (until.timeIntervalSinceNow > 0);
    XCTAssertNotNil(table, @"Missing foreground table with row %@. %@", label, self.app.debugDescription);
    // UIKit reports real offscreen cell frames even when all their child labels
    // have the same placeholder Y. Use the cell for scrolling and the final tap.
    XCUIElement *nav = nil;
    for (XCUIElement *candidate in self.app.navigationBars.allElementsBoundByIndex.reverseObjectEnumerator)
        if (candidate.hittable) { nav = candidate; break; }
    CGFloat previousY = CGFLOAT_MAX;
    for (NSUInteger i = 0; i < 40; ++i) {
        CGRect bounds = table.frame;
        CGFloat top = CGRectGetMinY(bounds) + 10;
        if (nav) top = MAX(top, CGRectGetMaxY(nav.frame) + 8);
        CGFloat bottom = CGRectGetMaxY(bounds) - 22;
        CGFloat y = CGRectGetMidY(row.frame);
        if (y >= top && y <= bottom && row.hittable) return row;
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
    XCUIElement *back = nil;
    CGFloat leading = CGRectGetMidX(bar.frame);
    for (XCUIElement *button in bar.buttons.allElementsBoundByIndex) {
        CGFloat x = CGRectGetMidX(button.frame);
        if (button.hittable && x < leading) { back = button; leading = x; }
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
    [field tap];
    NSMutableString *keys = [NSMutableString string];
    for (NSUInteger i=0; i<initial.length+1; ++i) [keys appendString:XCUIKeyboardKeyDelete];
    [field typeText:keys]; [field typeText:name];
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
    CGRect bounds = files.frame;
    CGFloat left = CGRectGetMinX(bounds) + 8, right = CGRectGetMaxX(bounds) - 8;
    CGFloat top = CGRectGetMinY(bounds) + 8, bottom = CGRectGetMaxY(bounds) - 12;
    XCUIElement *sidebar = self.app.collectionViews[@"Browse View"];
    XCUIElement *bar = self.app.navigationBars[@"FullDocumentManagerViewControllerNavigationBar"];
    XCUIElement *tabs = self.app.tabBars[@"DOC.browsingModeTabBar"];
    if (sidebar.exists && sidebar.hittable) left = MAX(left, CGRectGetMaxX(sidebar.frame) + 8);
    if (bar.exists) top = MAX(top, CGRectGetMaxY(bar.frame) + 8);
    if (tabs.exists) bottom = MIN(bottom, CGRectGetMinY(tabs.frame) - 8);
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
        if (item.exists && item.hittable && item.enabled) return item;
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
                    BOOL down = item.exists ? CGRectGetMidY(item.frame) >= CGRectGetMidY(files.frame) : scrolls < 4;
                    [self scrollFiles:files towardBottom:down];
                    ++scrolls;
                    ready = [NSDate dateWithTimeIntervalSinceNow:.4];
                }
            } else {
                XCUIElement *samples = [self fileNamed:@"UI Samples" inCollection:files];
                XCUIElement *place = nil;
                if (samples.exists && samples.hittable) { place = samples; stage = SKPickerSamples; }
                if (!place && stage < SKPickerAppFolder) {
                    XCUIElement *host = [self fileNamed:@"MOD Screen Test" inCollection:files];
                    if (host.exists && host.hittable) { place = host; stage = SKPickerAppFolder; }
                }
                if (!place && stage < SKPickerLocal && local.exists && local.hittable) {
                    place = local; stage = SKPickerLocal;
                }
                if (!place && stage < SKPickerBrowse && browse.exists && browse.hittable) {
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
                if (confirm.hittable && confirm.enabled) { [confirm tap]; return; }
            if (!self.app.collectionViews[@"File View"].exists &&
                !self.app.navigationBars[@"FullDocumentManagerViewControllerNavigationBar"].exists) return;
            [NSThread sleepForTimeInterval:.15];
        } while (until.timeIntervalSinceNow > 0);
    }
}
- (void)testActualSettingsModScreens {
    [self.app launch];
    [self capture:@"settings"];
    [self openHub];
    XCTAssertTrue(self.app.staticTexts[@"No MODs yet"].exists);
    [self capture:@"empty-hub"];
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
    XCTAssertTrue([self.app.navigationBars[@"Choose a colour"] waitForExistenceWithTimeout:15]);
    [[self findRow:@"Red"] tap];
    [self capture:@"fomod-options"];
    [self.app.navigationBars[@"Choose a colour"].buttons[@"Install"] tap];
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

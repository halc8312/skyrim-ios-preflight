/* SPDX-License-Identifier: GPL-3.0-only */
#import <XCTest/XCTest.h>
#import <CoreGraphics/CoreGraphics.h>

@interface ModScreensTests : XCTestCase
@property(nonatomic,strong) XCUIApplication *app;
@end

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

- (XCUIElement *)findRow:(NSString *)label {
    XCUIElement *table = self.app.tables.firstMatch;
    XCTAssertTrue([table waitForExistenceWithTimeout:12], @"No active table for %@", label);
    // UIKit reports real offscreen cell frames even when all their child labels
    // have the same placeholder Y. Use the cell for scrolling and the final tap.
    XCUIElement *row = [table.cells containingType:XCUIElementTypeStaticText identifier:label].firstMatch;
    XCTAssertTrue([row waitForExistenceWithTimeout:15], @"Missing row %@. %@", label, table.debugDescription);
    XCUIElement *nav = self.app.navigationBars.firstMatch;
    CGFloat previousY = CGFLOAT_MAX;
    for (NSUInteger i = 0; i < 16; ++i) {
        CGRect bounds = table.frame;
        CGFloat top = CGRectGetMinY(bounds) + 10;
        if (nav.exists) top = MAX(top, CGRectGetMaxY(nav.frame) + 8);
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
    [self capture:@"row-unavailable"];
    XCTFail(@"Could not bring the row %@ onscreen. %@", label, table.debugDescription);
    return row;
}
- (void)goBack {
    XCUIElement *bar = self.app.navigationBars.firstMatch;
    [bar.buttons.firstMatch tap];
}
- (void)openHub {
    XCUIElement *settings = self.app.navigationBars[@"SkyrimIOS Settings"];
    XCTAssertTrue([settings waitForExistenceWithTimeout:20]);
    [[self findRow:@"Manage MODs"] tap];
    XCTAssertTrue([self.app.navigationBars[@"MOD"] waitForExistenceWithTimeout:8]);
}
- (void)fillName:(NSString *)name replacing:(NSString *)initial {
    XCUIElement *field = self.app.alerts.textFields.firstMatch;
    XCTAssertTrue([field waitForExistenceWithTimeout:6]);
    [field tap];
    NSMutableString *keys = [NSMutableString string];
    for (NSUInteger i=0; i<initial.length+1; ++i) [keys appendString:XCUIKeyboardKeyDelete];
    [field typeText:keys]; [field typeText:name];
    XCTAssertEqualObjects(field.value,name);
    [self.app.alerts.buttons[@"OK"] tap];
}

// The system Files picker may initially show Recents, Browse, or the last
// directory. Select via its visible accessibility labels, including builds of
// Files that hide recognized extensions. No importer callback is substituted.
- (XCUIElement *)pickerItem:(NSString *)name {
    NSString *shortName = name.stringByDeletingPathExtension;
    NSPredicate *matches = [NSPredicate predicateWithFormat:
        @"label ==[c] %@ OR label ==[c] %@ OR label BEGINSWITH[c] %@ OR label BEGINSWITH[c] %@",
        name, shortName, [name stringByAppendingString:@","], [shortName stringByAppendingString:@","]];
    XCUIElementQuery *items = [[self.app descendantsMatchingType:XCUIElementTypeAny] matchingPredicate:matches];
    for (XCUIElement *item in items.allElementsBoundByIndex)
        if (item.hittable) return item;
    return nil;
}
- (void)pick:(NSString *)name folder:(BOOL)folder {
    // Start from an actual picker (as presented by the selected settings row).
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:25];
    BOOL sawBrowse=NO, sawLocal=NO, sawHost=NO, sawSamples=NO;
    XCUIElement *item=nil;
    while (!item && [deadline timeIntervalSinceNow]>0) {
        item = [self pickerItem:name];
        if (item) break;
        // Files can ignore its initial directory until the local provider has
        // appeared. Look up a fallback location only as far as the first match.
        XCUIElement *place;
        if (!sawSamples && (place=[self pickerItem:@"UI Samples"])) { [place tap]; sawSamples=YES; }
        else if (!sawHost && (place=[self pickerItem:@"MOD Screen Test"])) { [place tap]; sawHost=YES; }
        else if (!sawLocal && (place=[self pickerItem:@"On My iPhone"])) { [place tap]; sawLocal=YES; }
        else if (!sawBrowse && (place=[self pickerItem:@"Browse"])) { [place tap]; sawBrowse=YES; }
        else [NSThread sleepForTimeInterval:.2];
    }
    XCTAssertNotNil(item, @"Files could not locate %@: %@",name,self.app.debugDescription);
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
            if (self.app.navigationBars[@"MOD"].buttons[@"Reorder"].hittable) return;
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
    [self capture:@"save-check"]; [self goBack];

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
    [[self findRow:@"Blue"] tap];
    [self capture:@"fomod-options"];
    [self.app.buttons[@"Install"] tap];
    [self findRow:@"AuthoredInstaller"];

    [[self findRow:@"Default"] tap];
    [self.app.navigationBars.buttons[@"Add"] tap];
    [self fillName:@"Trial" replacing:@"MOD"];
    XCUIElement *trial = [self findRow:@"Trial"];
    [trial swipeLeft];
    if (![self.app.alerts[@"Rename"] waitForExistenceWithTimeout:1]) {
        XCUIElement *rename = self.app.buttons[@"Rename"];
        XCTAssertTrue([rename waitForExistenceWithTimeout:6]);
        [rename tap];
    }
    [self fillName:@"Trial Two" replacing:@"Trial"];
    [[self findRow:@"Trial Two"] tap]; [self capture:@"profile-renamed-selected"];
    [self goBack];
    [[self findRow:@"Find on Nexus Mods"] tap];
    XCTAssertTrue([self.app.navigationBars[@"Nexus Mods"] waitForExistenceWithTimeout:10]);
    XCTAssertTrue([self.app.webViews.firstMatch waitForExistenceWithTimeout:12]);
    [self capture:@"nexus-browser"]; [self goBack];

    [[self findRow:@"Share this MOD list"] tap];
    // The activity sheet is genuine. The simulator host later copies this
    // exact exported file to its Files documents for deterministic re-import.
    XCTAssertTrue([self.app.buttons[@"Copy"] waitForExistenceWithTimeout:10] ||
                  self.app.staticTexts[@"Copy"].exists || self.app.staticTexts[@"Save to Files"].exists ||
                  self.app.buttons[@"Save to Files"].exists || self.app.cells[@"Save to Files"].exists,
                  @"Expected the standard share sheet. %@",self.app.debugDescription);
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
    XCTAssertTrue(file.exists); [self findRow:file.label]; [self capture:@"conflict-files"]; [self goBack]; [self goBack];
    [[self findRow:@"Save check"] tap];
    [self findRow:@"No saves in this profile"];
    [self capture:@"separate-profile-saves"]; [self goBack];
    [[self findRow:@"Load a MOD list"] tap];
    [self pick:@"Trial Two.skyrimios-modlist.json" folder:NO];
    [[self findRow:@"Trial Two"] tap];
    [self findRow:@"Trial Two (imported)"];
    [self capture:@"imported-profile"];
}
@end

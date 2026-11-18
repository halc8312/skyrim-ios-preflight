/* SPDX-License-Identifier: GPL-3.0-only */
#import <XCTest/XCTest.h>

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

- (XCUIElement *)findRow:(NSString *)label direction:(BOOL)up {
    XCUIElement *table = self.app.tables.firstMatch;
    XCTAssertTrue([table waitForExistenceWithTimeout:12], @"No active table for %@", label);
    XCUIElement *row = table.staticTexts[label];
    for (NSUInteger i = 0; !row.hittable && i < 22; ++i) {
        if (up) [table swipeUp]; else [table swipeDown];
    }
    XCTAssertTrue(row.hittable, @"Table row %@ is unavailable. %@",label,table.debugDescription);
    return row;
}
- (void)goBack {
    XCUIElement *bar = self.app.navigationBars.firstMatch;
    [bar.buttons.firstMatch tap];
}
- (void)openHub {
    XCUIElement *settings = self.app.navigationBars[@"SkyrimIOS Settings"];
    XCTAssertTrue([settings waitForExistenceWithTimeout:20]);
    [[self findRow:@"Manage MODs" direction:YES] tap];
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
    NSArray *names = name.pathExtension.length ? @[name, name.stringByDeletingPathExtension] : @[name];
    for (NSString *n in names) {
        NSPredicate *matches = [NSPredicate predicateWithFormat:@"label ==[c] %@ OR label BEGINSWITH[c] %@",n,[n stringByAppendingString:@","]];
        for (XCUIElementQuery *query in @[self.app.cells, self.app.buttons, self.app.staticTexts, self.app.otherElements]) {
            XCUIElement *found = [query matchingPredicate:matches].firstMatch;
            if (found.exists && found.hittable) return found;
        }
    }
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
        XCUIElement *samples=[self pickerItem:@"UI Samples"];
        XCUIElement *host=[self pickerItem:@"MOD Screen Test"];
        XCUIElement *local=[self pickerItem:@"On My iPhone"];
        XCUIElement *browse=[self pickerItem:@"Browse"];
        if (!sawSamples && samples) { [samples tap]; sawSamples=YES; }
        else if (!sawHost && host) { [host tap]; sawHost=YES; }
        else if (!sawLocal && local) { [local tap]; sawLocal=YES; }
        else if (!sawBrowse && browse) { [browse tap]; sawBrowse=YES; }
        else [NSThread sleepForTimeInterval:.2];
    }
    XCTAssertNotNil(item, @"Files could not locate %@: %@",name,self.app.debugDescription);
    [self capture:[@"picker-" stringByAppendingString:name]];
    [item tap];
    // Folders must be opened then chosen; multi-select archives may require
    // an explicit Open. A single-select JSON picker dismisses on selection.
    XCUIElement *confirm = self.app.buttons[@"Open"];
    if ([confirm waitForExistenceWithTimeout:folder ? 6 : 1.5] && confirm.hittable) [confirm tap];
}
- (void)testActualSettingsModScreens {
    [self.app launch];
    [self capture:@"settings"];
    [self openHub];
    XCTAssertTrue(self.app.staticTexts[@"No MODs yet"].exists);
    [self capture:@"empty-hub"];
    [[self findRow:@"Save check" direction:YES] tap];
    XCTAssertTrue(self.app.staticTexts[@"Authored Save"].exists);
    XCTAssertTrue([self.app.staticTexts matchingPredicate:[NSPredicate predicateWithFormat:@"label CONTAINS %@",@"UnavailableFixture.esp"]].count>0);
    [self capture:@"save-check"]; [self goBack];

    [[self findRow:@"Add a zip file" direction:NO] tap];
    [self pick:@"AuthoredZip.zip" folder:NO];
    XCTAssertTrue([self.app.staticTexts[@"AuthoredZip"] waitForExistenceWithTimeout:15]);
    [self capture:@"zip-imported"];
    [[self findRow:@"Add a folder" direction:YES] tap];
    [self pick:@"AuthoredFolder" folder:YES];
    XCTAssertTrue([self.app.staticTexts[@"AuthoredFolder"] waitForExistenceWithTimeout:15]);
    [self capture:@"folder-imported"];
    [[self findRow:@"Add a zip file" direction:YES] tap];
    [self pick:@"AuthoredInstaller.zip" folder:NO];
    XCTAssertTrue([self.app.navigationBars[@"Choose a colour"] waitForExistenceWithTimeout:15]);
    [self.app.staticTexts[@"Blue"] tap];
    [self capture:@"fomod-options"];
    [self.app.buttons[@"Install"] tap];
    XCTAssertTrue([self.app.staticTexts[@"AuthoredInstaller"] waitForExistenceWithTimeout:15]);

    [[self findRow:@"Default" direction:NO] tap];
    [self.app.navigationBars.buttons[@"Add"] tap];
    [self fillName:@"Trial" replacing:@"MOD"];
    XCUIElement *trial=[self.app.cells containingType:XCUIElementTypeStaticText identifier:@"Trial"].firstMatch;
    XCTAssertTrue([trial waitForExistenceWithTimeout:6]); [trial swipeLeft];
    if (!self.app.alerts[@"Rename"].exists) [self.app.buttons[@"Rename"] tap];
    [self fillName:@"Trial Two" replacing:@"Trial"];
    [self.app.staticTexts[@"Trial Two"] tap]; [self capture:@"profile-renamed-selected"];
    [self goBack];
    [[self findRow:@"Find on Nexus Mods" direction:YES] tap];
    XCTAssertTrue([self.app.navigationBars[@"Nexus Mods"] waitForExistenceWithTimeout:10]);
    XCTAssertTrue([self.app.webViews.firstMatch waitForExistenceWithTimeout:12]);
    [self capture:@"nexus-browser"]; [self goBack];

    [[self findRow:@"Share this MOD list" direction:YES] tap];
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
    XCTAssertTrue(self.app.staticTexts[@"Trial Two"].exists);
    [[self findRow:@"Conflicts" direction:YES] tap];
    XCUIElement *conflict=[self.app.staticTexts matchingPredicate:[NSPredicate predicateWithFormat:@"label CONTAINS %@ AND label CONTAINS %@",@"AuthoredFolder",@"AuthoredZip"]].firstMatch;
    XCTAssertTrue([conflict waitForExistenceWithTimeout:6]); [self capture:@"conflict-summary"]; [conflict tap];
    XCUIElement *file=[self.app.staticTexts matchingPredicate:[NSPredicate predicateWithFormat:@"label ENDSWITH[c] %@",@"shared.txt"]].firstMatch;
    XCTAssertTrue(file.exists); [self capture:@"conflict-files"]; [self goBack]; [self goBack];
    [[self findRow:@"Save check" direction:YES] tap];
    XCTAssertTrue(self.app.staticTexts[@"No saves in this profile"].exists);
    [self capture:@"separate-profile-saves"]; [self goBack];
    [[self findRow:@"Load a MOD list" direction:YES] tap];
    [self pick:@"Trial Two.skyrimios-modlist.json" folder:NO];
    [[self findRow:@"Trial Two" direction:NO] tap];
    XCTAssertTrue([self.app.staticTexts[@"Trial Two (imported)"] waitForExistenceWithTimeout:10]);
    [self capture:@"imported-profile"];
}
@end

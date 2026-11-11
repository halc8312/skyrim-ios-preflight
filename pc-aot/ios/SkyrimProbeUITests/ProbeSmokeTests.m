#import <XCTest/XCTest.h>

@interface ProbeSmokeTests : XCTestCase
@end

@implementation ProbeSmokeTests
- (void)setUp { [super setUp]; self.continueAfterFailure = NO; }
- (void)waitForLog:(NSString *)text app:(XCUIApplication *)app {
    NSPredicate *predicate = [NSPredicate predicateWithFormat:@"value CONTAINS %@", text];
    XCTestExpectation *expectation = [self expectationForPredicate:predicate evaluatedWithObject:app.textViews[@"probe.log"] handler:nil];
    [self waitForExpectations:@[expectation] timeout:15];
}
- (void)capture:(NSString *)name {
    XCTAttachment *image = [XCTAttachment attachmentWithScreenshot:XCUIScreen.mainScreen.screenshot];
    image.name = name; image.lifetime = XCTAttachmentLifetimeKeepAlways; [self addAttachment:image];
}
- (void)dragViewport:(XCUIElement *)viewport left:(BOOL)left {
    CGFloat startX = left ? 0.2 : 0.65;
    XCUICoordinate *start = [viewport coordinateWithNormalizedOffset:CGVectorMake(startX, 0.6)];
    XCUICoordinate *end = [viewport coordinateWithNormalizedOffset:CGVectorMake(startX + 0.15, 0.35)];
    [start pressForDuration:0.1 thenDragToCoordinate:end];
}
- (void)testNativeProbeSmokeOnSimulator {
    XCUIApplication *app = [XCUIApplication new]; [app launch];
    XCUIElement *viewport = app.otherElements[@"probe.viewport"];
    XCUIElement *state = app.staticTexts[@"probe.state"];
    XCTAssertTrue([viewport waitForExistenceWithTimeout:15]); XCTAssertTrue(viewport.hittable);
    XCTAssertTrue(state.exists);
    [self waitForLog:@"SIMULATOR (not device evidence)" app:app];
    [self waitForLog:@"Metal pipeline ready" app:app];
    NSPredicate *uuidVisible = [NSPredicate predicateWithFormat:@"value MATCHES %@", @"(?s).*Main executable Mach-O UUID=[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}.*"];
    XCTAssertTrue([uuidVisible evaluateWithObject:app.textViews[@"probe.log"]], @"The running executable's actual LC_UUID must be logged.");
    [self capture:@"01-simulator-initial-metal-viewport"];

    NSString *initial = state.label;
    [self dragViewport:viewport left:YES];
    XCTAssertNotEqualObjects(state.label, initial, @"A real left-side pan must change probe movement state.");
    NSString *moved = state.label;
    [self dragViewport:viewport left:NO];
    XCTAssertNotEqualObjects(state.label, moved, @"A real right-side pan must change yaw state.");
    NSString *saved = state.label;
    [app.buttons[@"probe.button.save"] tap]; [self waitForLog:@"Saved probe state atomically" app:app];
    [self dragViewport:viewport left:YES]; XCTAssertNotEqualObjects(state.label, saved);
    [app.buttons[@"probe.button.load"] tap]; [self waitForLog:@"Loaded probe state" app:app];
    XCTAssertEqualObjects(state.label, saved);

    [app.buttons[@"probe.button.memoryProbe"] tap];
    NSPredicate *resultVisible = [NSPredicate predicateWithFormat:@"value CONTAINS 'VA PASS:' OR value CONTAINS 'VA FAIL:'"];
    XCTestExpectation *memory = [self expectationForPredicate:resultVisible evaluatedWithObject:app.textViews[@"probe.log"] handler:nil];
    [self waitForExpectations:@[memory] timeout:15];
    // A reported reservation failure is valid diagnostic behavior, not a memory feasibility pass.
    XCTAttachment *memoryLog = [XCTAttachment attachmentWithString:[app.textViews[@"probe.log"].value description]];
    memoryLog.name = @"simulator-va-result-pass-or-explicit-failure"; memoryLog.lifetime = XCTAttachmentLifetimeKeepAlways; [self addAttachment:memoryLog];
    [app.buttons[@"probe.button.tone"] tap]; [self waitForLog:@"Scheduled 250 ms 440 Hz tone" app:app];
    [self capture:@"02-simulator-controls-and-diagnostics"];

    [XCUIDevice.sharedDevice pressButton:XCUIDeviceButtonHome];
    XCTAssertTrue([app waitForState:XCUIApplicationStateRunningBackground timeout:10] || app.state == XCUIApplicationStateRunningBackgroundSuspended);
    [app activate]; XCTAssertTrue([app waitForState:XCUIApplicationStateRunningForeground timeout:10]);
    [self waitForLog:@"Background: state saved" app:app];
    [self waitForLog:@"Active: renderer resumed" app:app];
    XCTAssertEqualObjects(state.label, saved);
    [self capture:@"03-simulator-resumed"];

    [app terminate]; [app launch];
    XCTAssertTrue([state waitForExistenceWithTimeout:15]);
    [self waitForLog:@"Previous run marker:" app:app];
    [app.buttons[@"probe.button.load"] tap]; [self waitForLog:@"Loaded probe state" app:app];
    XCTAssertEqualObjects(state.label, saved, @"The atomic probe save must survive process relaunch.");
    [self capture:@"04-simulator-relaunch-probe-state"];
    XCTAttachment *finalLog = [XCTAttachment attachmentWithString:[app.textViews[@"probe.log"].value description]];
    finalLog.name = @"simulator-final-log"; finalLog.lifetime = XCTAttachmentLifetimeKeepAlways; [self addAttachment:finalLog];
}
@end

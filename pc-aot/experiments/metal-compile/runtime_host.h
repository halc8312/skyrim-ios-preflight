#pragma once
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Call on a worker thread. The caller owns scheduling and report persistence.
// Stop requests only prevent subsequent work: an issued Metal task is retained
// and joined on this worker before returning, and cannot be cancelled here.
// Progress dictionaries are immutable JSON snapshots and are delivered on the
// calling worker. Never update UIKit directly from the progress callback.
// sourcePath must name the authored phone_fixture.metal, with the original
// fixture.metal in the same directory for rejection/retention controls.
// repeats must be 4: three families x four semantic variants x four APIs x four
// balanced rotations (192 samples). Every API leads one variant per family.
// Progress timing counts only callbacks that returned normally; each snapshot
// contains prior completed totals. Final-return snapshot time is excluded.
FOUNDATION_EXPORT NSDictionary* SkyrimMetalRunPhoneComparison(
    NSString* sourcePath, NSUInteger repeats, BOOL metal4First,
    BOOL (^ _Nullable shouldContinue)(void),
    void (^ _Nullable progress)(NSDictionary* snapshot));

NS_ASSUME_NONNULL_END

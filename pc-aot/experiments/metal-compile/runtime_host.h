#pragma once
#ifdef __cplusplus
#include <cstdint>
namespace SkyrimPhoneProgress {
inline constexpr uint64_t SnapshotStride = 4;
enum class Boundary { Operation, SampleComplete, Control };
inline constexpr bool fullSnapshot(Boundary boundary, uint64_t completedSamples) noexcept {
    return boundary == Boundary::Control ||
        (boundary == Boundary::SampleComplete && completedSamples != 0 && completedSamples % SnapshotStride == 0);
}
} // namespace SkyrimPhoneProgress
#endif

#ifdef __OBJC__
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
// Full progress snapshots are emitted every four completed samples and at
// control boundaries. Stop checks remain at every operation boundary. A forced
// kill before persistence completes may lose up to four completed serial samples
// plus current unfinished work since the last successfully persisted snapshot.
// This bound assumes a functioning persistence callback; no callback means no
// intermediate persistence. Controlled stop/error returns the complete in-memory
// report for the host's terminal save. A separate authored MRC scheduling control
// follows serial controls and has its own nested result and eligibility status.
FOUNDATION_EXPORT NSDictionary* SkyrimMetalRunPhoneComparison(
    NSString* sourcePath, NSUInteger repeats, BOOL metal4First,
    BOOL (^ _Nullable shouldContinue)(void),
    void (^ _Nullable progress)(NSDictionary* snapshot));

NS_ASSUME_NONNULL_END
#endif

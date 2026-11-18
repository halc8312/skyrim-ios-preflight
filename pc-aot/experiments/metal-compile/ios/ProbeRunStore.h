#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Owns authored probe records only. Never reads the Skyrim application container.
// Methods are serialized internally; a finished record cannot be overwritten.
@interface ProbeRunStore : NSObject
@property(nonatomic, readonly) NSURL *rootURL;
@property(nonatomic, readonly, nullable) NSURL *latestReportURL;
- (nullable instancetype)initWithRootURL:(NSURL *)rootURL error:(NSError * _Nullable * _Nullable)error;
- (nullable NSURL *)beginRunWithMetal4First:(BOOL)metal4First error:(NSError * _Nullable * _Nullable)error;
- (BOOL)writeSnapshot:(NSDictionary *)snapshot
               toURL:(NSURL *)reportURL
            finished:(BOOL)finished
               error:(NSError * _Nullable * _Nullable)error;
// Complete writeSnapshot wall time, including validation, serialization and I/O.
// The measurement is returned after the call; it is not inserted into that write.
- (BOOL)writeSnapshot:(NSDictionary *)snapshot
               toURL:(NSURL *)reportURL
            finished:(BOOL)finished
  elapsedNanoseconds:(uint64_t * _Nullable)elapsed
               error:(NSError * _Nullable * _Nullable)error;
- (nullable NSDictionary *)readRecordAtURL:(NSURL *)reportURL error:(NSError * _Nullable * _Nullable)error;
@end

NS_ASSUME_NONNULL_END

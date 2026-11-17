// Authored Foundation/MRC ownership boundary; no Metal device or real task.
#import <Foundation/Foundation.h>
#import <Block.h>

#include "owner_retirement.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/stat.h>

#if __has_feature(objc_arc)
#error This fixture must be compiled with -fno-objc-arc
#endif

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<class T> class Held {
public:
    Held() = default;
    explicit Held(T value) : value_([value retain]) {}
    Held(const Held& other) : Held(other.value_) {}
    Held(Held&& other) noexcept : value_(other.value_) { other.value_ = nil; }
    Held& operator=(Held other) noexcept { std::swap(value_, other.value_); return *this; }
    ~Held() { [value_ release]; }
    T get() const { return value_; }
    void reset() { Held empty; std::swap(value_, empty.value_); }
private:
    T value_ = nil;
};

struct Counts {
    std::atomic<unsigned> owners{0}, tasks{0}, tokens{0};
    std::atomic<unsigned> ownersCreated{0}, tasksCreated{0}, tokensCreated{0};
};

@interface LifetimeToken : NSObject <NSCopying> {
    Counts* _counts;
}
- (instancetype)initWithCounts:(Counts*)counts;
@end
@implementation LifetimeToken
- (instancetype)initWithCounts:(Counts*)counts {
    if ((self = [super init])) { _counts = counts; ++_counts->tokens; ++_counts->tokensCreated; }
    return self;
}
- (id)copyWithZone:(NSZone*)zone { (void)zone; return [self retain]; }
- (void)dealloc { --_counts->tokens; [super dealloc]; }
@end

@interface LifetimeOwner : NSObject {
    Counts* _counts;
    NSString* _source;
    NSDictionary* _options;
}
- (instancetype)initWithCounts:(Counts*)counts source:(NSString*)source options:(NSDictionary*)options;
- (BOOL)snapshotMatches;
@end
@implementation LifetimeOwner
- (instancetype)initWithCounts:(Counts*)counts source:(NSString*)source options:(NSDictionary*)options {
    if ((self = [super init])) {
        _counts = counts; ++_counts->owners; ++_counts->ownersCreated;
        _source = [source copy];
        // Fixture dictionary values implement NSCopying. This is not a claim
        // that arbitrary runtime option graphs support a generic deep copy.
        _options = [[NSDictionary alloc] initWithDictionary:options copyItems:YES];
    }
    return self;
}
- (BOOL)snapshotMatches {
    return [_source isEqualToString:@"authored-source"] &&
        [[_options objectForKey:@"mode"] isEqualToString:@"authored-option"] &&
        [[_options objectForKey:@"lifetime"] isKindOfClass:[LifetimeToken class]];
}
- (void)dealloc { [_source release]; [_options release]; --_counts->owners; [super dealloc]; }
@end

@interface LifetimeTask : NSObject {
    Counts* _counts;
}
- (instancetype)initWithCounts:(Counts*)counts;
@end
@implementation LifetimeTask
- (instancetype)initWithCounts:(Counts*)counts {
    if ((self = [super init])) { _counts = counts; ++_counts->tasks; ++_counts->tasksCreated; }
    return self;
}
- (void)dealloc { --_counts->tasks; [super dealloc]; }
@end

struct Payload { Held<NSString*> value; Held<NSError*> error; };
using Bridge = metal_compile_experiment::OwnerRetirement<Payload, Held<LifetimeTask*>>;
typedef void (^Completion)(NSString*, NSError*);
struct Observation {
    Counts counts;
    unsigned notifications = 0, callbackAttempts = 0, faults = 0, retirements = 0;
    unsigned launchDepth = 0;
    bool callerPoolExpired = false, launchExited = false, snapshotSurvived = false;
    bool notificationBeforeLaunchExit = false, retiredAfterLaunchExit = false;
    bool retirementInsideNotification = false, notifying = false;
    bool timeoutDidNotCancel = false, exceptionObserved = false;
    Payload first;
};
struct LaunchScope {
    Observation& value;
    explicit LaunchScope(Observation& observation) : value(observation) { ++value.launchDepth; }
    ~LaunchScope() { --value.launchDepth; }
};
struct CopiedCompletion {
    Completion value = nil;
    ~CopiedCompletion() { if (value) Block_release(value); }
    void clear() { if (value) Block_release(value); value = nil; }
};

static NSError* authoredError(NSInteger code) {
    return [NSError errorWithDomain:@"AuthoredLifetime" code:code
                           userInfo:@{NSLocalizedDescriptionKey: @"retained authored diagnostic"}];
}

static NSDictionary* runCase(NSString* name) {
    const bool late = [name isEqualToString:@"late_after_destroy_request"];
    const bool warning = [name isEqualToString:@"early_warning"];
    const bool nilFault = [name hasPrefix:@"nil_"];
    const bool throws = [name hasPrefix:@"throw_"];
    const bool afterSuccess = [name hasSuffix:@"after_success"];
    const bool failure = nilFault || throws;
    auto observation = std::make_shared<Observation>();
    std::shared_ptr<Bridge> bridge;
    CopiedCompletion completion;

    @autoreleasepool {
        NSMutableString* source = [NSMutableString stringWithString:@"authored-source"];
        NSMutableString* mode = [NSMutableString stringWithString:@"authored-option"];
        LifetimeToken* inputToken = [[[LifetimeToken alloc] initWithCounts:&observation->counts] autorelease];
        NSDictionary* options = @{ @"mode": mode, @"lifetime": inputToken };
        LifetimeOwner* raw = [[[LifetimeOwner alloc] initWithCounts:&observation->counts source:source options:options] autorelease];
        Held<LifetimeOwner*> owner(raw);
        bridge = Bridge::create(
            [observation, raw](Payload payload) {
                observation->notifying = true;
                require(observation->callerPoolExpired && [raw snapshotMatches], "input snapshot expired or changed");
                observation->snapshotSurvived = true;
                observation->notificationBeforeLaunchExit = !observation->launchExited;
                ++observation->notifications;
                observation->first = std::move(payload);
                observation->notifying = false;
            },
            [observation, owner](Held<LifetimeTask*> task) mutable {
                ++observation->retirements;
                observation->retiredAfterLaunchExit = observation->launchExited && observation->launchDepth == 0;
                observation->retirementInsideNotification = observation->notifying;
                // Real Metal task release/join rules are intentionally absent.
                owner.reset();
                task.reset();
            });
        LifetimeToken* blockToken = [[[LifetimeToken alloc] initWithCounts:&observation->counts] autorelease];
        auto capturedBridge = bridge;
        auto capturedObservation = observation;
        completion.value = Block_copy(^(NSString* result, NSError* error) {
            require([blockToken isKindOfClass:[LifetimeToken class]], "copied block lost capture");
            ++capturedObservation->callbackAttempts;
            capturedBridge->callback(Payload{Held<NSString*>(result), Held<NSError*>(error)});
            // No owner dereference after callback may have triggered retirement.
        });
        [source setString:@"caller-mutated-source"];
        [mode setString:@"caller-mutated-option"];
        require([raw snapshotMatches], "authored input copy is not independent");
    }
    observation->callerPoolExpired = true;
    require(observation->counts.owners == 1 && observation->counts.tokens == 2,
            "caller pool did not leave exactly the retained owner and block captures");

    Held<LifetimeTask*> task;
    @autoreleasepool {
        @try {
            LaunchScope launch(*observation);
            if (!late && (!failure || afterSuccess)) {
                completion.value([NSString stringWithFormat:@"authored-%@", @"result"], warning ? authoredError(7) : nil);
                bridge->requestDestroy();
                require(observation->notifications == 1 && observation->retirements == 0,
                        "notification was delayed or owner retired during launch");
            }
            if (throws) [NSException raise:@"AuthoredProviderFailure" format:@"fake provider exception"];
            if (nilFault) {
                ++observation->faults;
                completion.value(nil, authoredError(11));
                bridge->requestDestroy();
            } else if (!throws) {
                LifetimeTask* returned = [[[LifetimeTask alloc] initWithCounts:&observation->counts] autorelease];
                task = Held<LifetimeTask*>(returned);
            }
        } @catch (NSException* exception) {
            require([[exception name] isEqualToString:@"AuthoredProviderFailure"], "unexpected Objective-C exception");
            observation->exceptionObserved = true;
            ++observation->faults;
            completion.value(nil, authoredError(12));
            bridge->requestDestroy();
        }
        if (late) {
            // Logical timeout signal, not a timer or a cancellation request.
            bridge->requestDestroy();
            observation->timeoutDidNotCancel = observation->notifications == 0 && observation->retirements == 0;
        }
        require(observation->retirements == 0, "owner retired before launch pool drained");
    }
    require(observation->launchDepth == 0, "provider scope did not unwind");
    observation->launchExited = true;
    bridge->finishLaunch(std::move(task));
    if (late) {
        require(observation->retirements == 0 && observation->counts.owners == 1,
                "timeout destroyed owner before late callback");
        std::exception_ptr error;
        std::thread callbackThread([&] {
            @autoreleasepool {
                try {
                    completion.value([NSString stringWithFormat:@"authored-%@", @"result"], authoredError(7));
                } catch (...) { error = std::current_exception(); }
            }
        });
        callbackThread.join();
        if (error) std::rethrow_exception(error);
    }
    require(observation->notifications == 1 && observation->retirements == 1 && bridge->retired(),
            "notification/retirement must occur exactly once");
    require(observation->retiredAfterLaunchExit && !observation->retirementInsideNotification,
            "retirement crossed an active launch/notification scope");
    require(observation->counts.owners == 0 && observation->counts.tasks == 0 && observation->counts.tokens == 1,
            "retirement did not release owned Foundation objects");
    const bool success = !failure || afterSuccess;
    require(success ? [observation->first.value.get() isEqualToString:@"authored-result"] : observation->first.value.get() == nil,
            "first published payload was overwritten or expired");
    const NSInteger expectedCode = warning || late ? 7 : !success ? (throws ? 12 : 11) : 0;
    NSError* retainedError = observation->first.error.get();
    require(expectedCode ? retainedError && [retainedError code] == expectedCode &&
                [[retainedError domain] isEqualToString:@"AuthoredLifetime"] &&
                [[retainedError localizedDescription] isEqualToString:@"retained authored diagnostic"]
            : retainedError == nil, "first NSError/warning was overwritten or expired");
    require(observation->faults == (failure ? 1u : 0u), "post-notification fault was hidden");
    completion.clear();
    bridge.reset();
    observation->first = Payload{};
    require(observation->counts.tokens == 0, "copied block did not release its Objective-C capture");
    return @{ @"case": name, @"notifications": @(observation->notifications),
              @"callback_attempts": @(observation->callbackAttempts), @"faults": @(observation->faults),
              @"retirements": @(observation->retirements), @"snapshot_survived_caller_pool": @(observation->snapshotSurvived),
              @"first_payload_preserved": @YES, @"error_code": @(expectedCode),
              @"notification_before_launch_exit": @(observation->notificationBeforeLaunchExit),
              @"retirement_after_launch_exit": @(observation->retiredAfterLaunchExit),
              @"retirement_inside_notification": @(observation->retirementInsideNotification),
              @"timeout_did_not_cancel": @(observation->timeoutDidNotCancel),
              @"objc_exception_observed": @(observation->exceptionObserved),
              @"tracked_objects_balanced": @YES,
              @"objects_created": @{ @"owner": @(observation->counts.ownersCreated.load()),
                                      @"task": @(observation->counts.tasksCreated.load()),
                                      @"token": @(observation->counts.tokensCreated.load()) } };
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc != 3 || std::string(argv[1]) != "--report") { std::fprintf(stderr, "usage: mrc-lifetime --report NEW.json\n"); return 2; }
        struct stat existing {};
        if (lstat(argv[2], &existing) == 0) { std::fprintf(stderr, "report entry already exists\n"); return 2; }
        if (errno != ENOENT) { std::perror("report path check"); return 2; }
        NSString* path = [NSString stringWithUTF8String:argv[2]];
        if (!path) { std::fprintf(stderr, "report path must be valid UTF-8\n"); return 2; }
        NSMutableDictionary* report = [NSMutableDictionary dictionaryWithDictionary:@{
            @"schema": @1, @"scope": @"authored-foundation-mrc-owner-retirement", @"status": @"running",
            @"manual_reference_counting": @YES, @"foundation_ownership_verified": @NO,
            @"tracked_object_scope": @"LifetimeOwner/LifetimeTask/LifetimeToken only; not a Foundation-wide leak proof",
            @"metal_api_verified": @NO, @"metal_task_semantics_verified": @NO,
            @"gpu_execution_verified": @NO, @"production_adapter_verified": @NO }];
        NSMutableArray* cases = [NSMutableArray array];
        report[@"cases"] = cases;
        @try {
            try {
                for (NSString* name in @[ @"early_success", @"late_after_destroy_request", @"early_warning",
                                          @"nil_before_callback", @"throw_before_callback", @"nil_after_success", @"throw_after_success" ]) {
                    [cases addObject:runCase(name)];
                }
                report[@"status"] = @"passed";
                report[@"foundation_ownership_verified"] = @YES;
            } catch (const std::exception& error) {
                report[@"status"] = @"failed";
                report[@"reason"] = [NSString stringWithUTF8String:error.what()];
            }
        } @catch (NSException* exception) {
            report[@"status"] = @"failed";
            report[@"reason"] = [NSString stringWithFormat:@"%@: %@", [exception name], [exception reason]];
        }
        NSError* error = nil;
        NSData* data = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:&error];
        if (!data || ![data writeToFile:path options:NSDataWritingAtomic error:&error]) {
            std::fprintf(stderr, "report write failed: %s\n", [[error description] UTF8String]); return 1;
        }
        return [report[@"status"] isEqualToString:@"passed"] ? 0 : 1;
    }
}

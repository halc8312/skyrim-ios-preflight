/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#import <Foundation/Foundation.h>

typedef NS_ENUM(NSInteger, SkyrimDiagnosticStatus) {
    SkyrimDiagnosticSent = 0,
    SkyrimDiagnosticNotConfigured,
    SkyrimDiagnosticBusy,
    SkyrimDiagnosticCaptureFailed,
    SkyrimDiagnosticConnectionFailed,
    SkyrimDiagnosticRejected,
    SkyrimDiagnosticInvalidReceipt
};

/* Startup only. The log is private to this process; paths never enter reports. */
void skyrim_diagnostics_set_log_path(const char *path);
BOOL skyrim_diagnostics_available(void);
#ifdef SKYRIM_IOS_CLASSIC_SKSE
/* Local share only, restricted to the two numeric Classic records. */
void skyrim_diagnostics_share_classic(void (^completion)(NSString *text));
#endif
/* Main-thread request; all file/network work runs off the game thread. Only an
 * explicit user action calls this. Completion is always on the main queue. */
void skyrim_diagnostics_send(void (^completion)(SkyrimDiagnosticStatus, NSString *));

/* Foundation-only boundaries also exercised on the Apple validation host. */
BOOL skyrim_diagnostics_valid_pairing(NSDictionary *pairing);
NSDictionary *skyrim_diagnostics_capture(NSString *directory, NSString *current,
                                        NSUInteger build, NSError **error);
NSData *skyrim_diagnostics_encode(NSDictionary *report, NSError **error);

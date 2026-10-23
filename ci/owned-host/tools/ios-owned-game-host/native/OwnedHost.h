#ifndef IOS_OWNED_GAME_HOST_H
#define IOS_OWNED_GAME_HOST_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Experimental host. Version 0x10000, sizeof=96. All readiness /
 * authentication / iPhone verification flags remain zero. Numeric module and
 * ordinal describe the first failure; no guest pointers or arguments escape. */
typedef struct OHStatusV1 {
    uint32_t version,bytes,state,policy;
    uint32_t failure,missing_module,missing_ordinal,exit_code;
    uint64_t function_imports,registered_functions,variable_imports,supported_variables;
    uint64_t elapsed_ms,function_count;
    uint32_t ready_to_play,signature_authenticated,iphone_game_verified,reserved;
} OHStatusV1;
/* Must run on a worker, once per process. UTF8 path byte ranges are valid,
 * immutable and disjoint from token; lengths exclude NUL. Game is inside the
 * app sandbox and mounted read-only. Content must not exist and is disjoint.
 * Fixed main-bundle OwnedGuestBinding.json is the expected metadata source.
 * policy=0 StrictAll; policy=1 explicit fatal-on-first-unknown diagnosis.
 * 0 success; -1 invalid input; -2 repeated/invalid state; -3 preparation error.
 * Failure is terminal; the app must restart for another attempt. */
int32_t OHPrepareV1(const uint8_t *game,size_t game_bytes,
                    const uint8_t *content,size_t content_bytes,
                    uint32_t policy,uint64_t *token);
/* Renderer must be attached first. 1..600 seconds; deadline ends the process
 * via _exit. This is a bounded startup diagnosis, never an unrestricted play
 * session. No guest call executes on the Swift calling thread. */
int32_t OHStartV1(uint64_t token,uint32_t max_seconds);
/* Normal foreground play, with the same preparation/renderer/one-shot checks.
 * No scheduled process deadline. Fatal errors still terminate after evidence;
 * iOS may suspend or terminate the app. No guest call uses the Swift thread. */
int32_t OHPlayV1(uint64_t token);
/* Report an unrecoverable UI/renderer failure during an executing session.
 * Starts the existing bounded fatal-evidence/exit path once; it never waits on
 * evidence I/O on the caller. Invalid token/pre-start/repeated calls refuse. */
int32_t OHAbortFailedSessionV1(uint64_t token);
/* token=0 permits a numeric observation before prepare returned its token. */
int32_t OHReadStatusV1(uint64_t token,OHStatusV1 *out,size_t bytes);
/* Complete ordinary port0 snapshot. Invalid/repeated lifecycle is refused;
 * no guest memory/context pointer is accepted. Reserved button0x0800 refused. */
int32_t OHUpdatePadV1(uint64_t token,uint16_t buttons,uint8_t left_trigger,
                     uint8_t right_trigger,int16_t lx,int16_t ly,int16_t rx,int16_t ry);
#ifdef __cplusplus
}
static_assert(sizeof(OHStatusV1)==96,"OHStatus ABI");
#endif
#endif

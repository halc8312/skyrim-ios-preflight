/* SPDX-License-Identifier: GPL-3.0-only */
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "mod_ece_profile.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned digest_calls;
static const char *python;

/* The synthetic profile cases inject successful digest observations. */
static bool replay_digest(int fd, size_t length, uint8_t result[32]) {
    assert(length >= 32);
    ++digest_calls;
    return read(fd, result, 32) == 32;
}

static bool hash_file(int fd, size_t length, uint8_t result[32]) {
    ++digest_calls;
    int channel[2];
    assert(pipe(channel) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(channel[0]);
        assert(dup2(fd, STDIN_FILENO) == STDIN_FILENO);
        assert(dup2(channel[1], STDOUT_FILENO) == STDOUT_FILENO);
        close(channel[1]);
        char bound[32];
        snprintf(bound, sizeof bound, "%zu", length);
        execl(python, python, "-c",
              "import hashlib, sys; n = int(sys.argv[1]); data = sys.stdin.buffer.read(n + 1); "
              "assert len(data) == n; sys.stdout.buffer.write(hashlib.sha256(data).digest())",
              bound, (char *)NULL);
        _exit(2);
    }
    close(channel[1]);
    FILE *reader = fdopen(channel[0], "rb");
    assert(reader);
    bool success = fread(result, 1, 32, reader) == 32 && fgetc(reader) == EOF && !ferror(reader);
    fclose(reader);
    int status;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 && success;
}

int main(int argc, char **argv) {
    assert(argc == 3 || argc == 4);
    const bool real = !strcmp(argv[2], "sha256");
    assert((real && argc == 4) || (!real && argc == 3 && !strcmp(argv[2], "replay")));
    if (real) python = argv[3];

    int data = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    assert(data >= 0);
    SkyrimEceProfileStatus status = skyrim_ece_profile_check_female_nord(data, real ? hash_file : replay_digest);
    close(data);
    printf("%u %u\n", (unsigned)status, digest_calls);
    return 0;
}

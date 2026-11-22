/* SPDX-License-Identifier: GPL-3.0-only */
#include "classic_mod_layout.h"
#include "highheel_identity.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum { HANDLER = 0x10000u, FILE_BASE = 0x20000u, FILE_STRIDE = 0x500u };
static uint8_t handler[CLASSIC_LOADED_MODS + 4 * SKSE_CLASSIC_MOD_LIMIT];
static uint8_t files[SKSE_CLASSIC_MOD_LIMIT][FILE_STRIDE];
static uint8_t singleton[4];
static size_t count;
static unsigned hashes;
static const char *python;

static void put_word(uint8_t *bytes, uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(word >> (i * 8));
}

static bool read_guest(void *unused, uint32_t address, void *out, size_t size) {
    (void)unused;
    if (address == CLASSIC_DATA_HANDLER && size == sizeof singleton) {
        memcpy(out, singleton, size);
        return true;
    }
    if (address >= HANDLER && (uint64_t)address + size <= HANDLER + sizeof handler) {
        memcpy(out, handler + (address - HANDLER), size);
        return true;
    }
    if (address >= FILE_BASE) {
        uint32_t relative = address - FILE_BASE, row = relative / FILE_STRIDE, offset = relative % FILE_STRIDE;
        if (row < count && size <= FILE_STRIDE - offset) {
            memcpy(out, files[row] + offset, size);
            return true;
        }
    }
    return false;
}

static bool replay_digest(const void *bytes, size_t length, uint8_t digest[32]) {
    assert(length >= 32);
    ++hashes;
    memcpy(digest, bytes, 32);
    return true;
}

static bool python_digest(const void *bytes, size_t length, uint8_t digest[32]) {
    ++hashes;
    FILE *input = tmpfile();
    assert(input && fwrite(bytes, 1, length, input) == length && fflush(input) == 0);
    rewind(input);
    int channel[2];
    assert(pipe(channel) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(channel[0]);
        assert(dup2(fileno(input), STDIN_FILENO) == STDIN_FILENO);
        assert(dup2(channel[1], STDOUT_FILENO) == STDOUT_FILENO);
        close(channel[1]);
        execl(python, python, "-c",
              "import hashlib,sys; sys.stdout.buffer.write(hashlib.sha256(sys.stdin.buffer.read()).digest())",
              (char *)NULL);
        _exit(2);
    }
    fclose(input);
    close(channel[1]);
    FILE *reader = fdopen(channel[0], "rb");
    assert(reader);
    bool success = fread(digest, 1, 32, reader) == 32 && fgetc(reader) == EOF && !ferror(reader);
    fclose(reader);
    int status;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status) && success;
}

int main(int argc, char **argv) {
    assert(argc >= 4);
    if (strcmp(argv[2], "-")) python = argv[2];
    const HighHeelDigest digest = python ? python_digest : replay_digest;
    const int data = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    assert(data >= 0);
    if (argc == 4) {
        const HighHeelIdentityResult result = highheel_identity_check_assets(data, digest);
        close(data);
        printf("%u %u\n", (unsigned)result, hashes);
        return 0;
    }
    const unsigned expected_slot = (unsigned)strtoul(argv[3], NULL, 10);
    count = (size_t)argc - 4;
    assert(count <= SKSE_CLASSIC_MOD_LIMIT);
    put_word(singleton, HANDLER);
    put_word(handler + CLASSIC_LOADED_COUNT, (uint32_t)count);
    for (unsigned i = 0; i < count; ++i) {
        assert(strlen(argv[i + 4]) < CLASSIC_FILE_NAME_BYTES);
        strcpy((char *)files[i] + CLASSIC_FILE_NAME, argv[i + 4]);
        files[i][CLASSIC_FILE_INDEX] = (uint8_t)i;
        put_word(handler + CLASSIC_LOADED_MODS + 4 * i, FILE_BASE + FILE_STRIDE * i);
    }

    ClassicModCatalog *catalog = NULL;
    ClassicModCatalogResult captured = classic_mod_catalog_capture(read_guest, NULL, &catalog);
    uint32_t setting = UINT32_MAX;
    const HighHeelIdentityResult result = highheel_identity_join(data, catalog, digest, &setting);
    close(data);
    classic_mod_catalog_free(catalog);
    if (result == HIGHHEEL_IDENTITY_MATCH) {
        assert(captured == CLASSIC_MOD_CATALOG_OK && expected_slot < SKSE_CLASSIC_MOD_LIMIT);
        assert(setting == ((uint32_t)expected_slot << 24 | 0x800u));
    } else {
        assert(setting == UINT32_MAX);
    }
    printf("%u %u\n", (unsigned)result, hashes);
    return 0;
}

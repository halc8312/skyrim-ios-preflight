/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_winfile.h"
#include <stdlib.h>
#include <string.h>

enum {
    FILE_NOT_FOUND = 2, PATH_NOT_FOUND = 3, TOO_MANY_OPEN_FILES = 4, ACCESS_DENIED = 5,
    NOT_ENOUGH_MEMORY = 8, INVALID_DATA = 13, SHARING_VIOLATION = 32, FILE_EXISTS = 80,
    WRITE_FAULT = 29, INVALID_PARAMETER = 87, ALREADY_EXISTS = 183, FILE_TOO_LARGE = 223,
    OPERATION_ABORTED = 995,
    MAX_OPEN_ATTEMPTS = 32, MAX_DIRECTORY_ATTEMPTS = 32, MAX_OPENED_BYTES = 4 * 1024 * 1024
};

static bool finish_component(const char *path, size_t first, size_t *end) {
    const size_t length = *end - first;
    if (!length) return false;
    if (path[first] == '.' && length == 1) {
        *end = first ? first - 1 : 0;
        return true;
    }
    if (path[first] == '.' && length == 2 && path[first + 1] == '.') {
        if (!first) return false; // A parent outside the granted virtual root.
        size_t previous = first - 1;
        while (previous && path[previous - 1] != '/') --previous;
        *end = previous ? previous - 1 : 0;
        return true;
    }
    // Windows may remove trailing spaces and dots. Keep one unambiguous name.
    return path[*end - 1] != ' ' && path[*end - 1] != '.';
}

static size_t write_utf8(char *out, uint32_t value) {
    if (value < 0x80) {
        out[0] = (char)value;
        return 1;
    }
    if (value < 0x800) {
        out[0] = (char)(0xc0 | value >> 6);
        out[1] = (char)(0x80 | (value & 63));
        return 2;
    }
    if (value < 0x10000) {
        out[0] = (char)(0xe0 | value >> 12);
        out[1] = (char)(0x80 | (value >> 6 & 63));
        out[2] = (char)(0x80 | (value & 63));
        return 3;
    }
    out[0] = (char)(0xf0 | value >> 18);
    out[1] = (char)(0x80 | (value >> 12 & 63));
    out[2] = (char)(0x80 | (value >> 6 & 63));
    out[3] = (char)(0x80 | (value & 63));
    return 4;
}

bool skse_win_asset_path(const uint16_t *units, size_t length, bool wide, char path[SKSE_WIN_PATH_BYTES]) {
    if (length >= SKSE_WIN_PATH_UNITS) return false;
    static const char root[] = SKSE_WIN_VIRTUAL_GAME_ROOT;
    size_t relative = 0;
    if (length >= 2 && units[1] == ':') {
        if (length < sizeof root - 1) return false;
        for (; relative < sizeof root - 1; ++relative) {
            unsigned unit = units[relative] == '/' ? '\\' : units[relative];
            if (skse_win_ascii_lower(unit) != skse_win_ascii_lower((uint8_t)root[relative])) return false;
        }
    }
    size_t end = 0, start = 0;
    for (size_t i = relative; i < length; ++i) {
        uint32_t value = units[i];
        if (value == '/' || value == '\\') {
            if (!finish_component(path, start, &end) || end + 1 >= SKSE_WIN_PATH_BYTES) return false;
            if (end) path[end++] = '/';
            start = end;
            continue;
        }
        if (value < 0x20 || value == 0x7f || value == ':' || value == '*' || value == '?' ||
            value == '"' || value == '<' || value == '>' || value == '|' ||
            (!wide && value > 0x7e)) return false;
        if (value >= 0xd800 && value <= 0xdfff) {
            if (value >= 0xdc00 || i + 1 == length || units[i + 1] < 0xdc00 || units[i + 1] > 0xdfff)
                return false;
            value = 0x10000 + ((value - 0xd800) << 10) + (units[++i] - 0xdc00);
        }
        char encoded[4];
        size_t count = write_utf8(encoded, value);
        if (count >= SKSE_WIN_PATH_BYTES - end) return false;
        for (size_t j = 0; j < count; ++j) path[end++] = encoded[j];
    }
    if (!finish_component(path, start, &end) || !end) return false;
    path[end] = '\0';
    return true;
}

static char *copy_path(const char *path) {
    size_t length = strlen(path) + 1;
    char *copy = malloc(length);
    if (copy) memcpy(copy, path, length);
    return copy;
}

static bool ini_space(unsigned c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static SkseWinIniValue trim_ini(SkseWinIniValue value) {
    while (value.length && ini_space(*value.bytes)) { ++value.bytes; --value.length; }
    while (value.length && ini_space(value.bytes[value.length - 1])) --value.length;
    return value;
}

static bool ini_name_equal(SkseWinIniValue name, const char *expected, size_t length) {
    if (name.length != length) return false;
    for (size_t i = 0; i < length; ++i)
        if (skse_win_ascii_lower(name.bytes[i]) != skse_win_ascii_lower((uint8_t)expected[i])) return false;
    return true;
}

SkseWinIniValue skse_win_ini_lookup(const SkseWinFileData *file, const char *section, const char *key) {
    const SkseWinIniValue missing = {0};
    if (!file->length || memchr(file->bytes, 0, file->length)) return missing;
    const uint8_t *cursor = file->bytes, *end = cursor + file->length;
    const size_t section_length = strlen(section), key_length = strlen(key);
    bool selected = false;
    while (cursor < end) {
        const uint8_t *newline = memchr(cursor, '\n', (size_t)(end - cursor));
        const uint8_t *line_end = newline ? newline : end;
        SkseWinIniValue line = trim_ini((SkseWinIniValue){cursor, (size_t)(line_end - cursor)});
        cursor = newline ? newline + 1 : end;
        if (!line.length || *line.bytes == ';') continue;
        if (*line.bytes == '[') {
            if (selected) break;
            const uint8_t *close = memchr(line.bytes + 1, ']', line.length - 1);
            if (!close) continue;
            const SkseWinIniValue name = trim_ini((SkseWinIniValue){line.bytes + 1, (size_t)(close - line.bytes - 1)});
            selected = ini_name_equal(name, section, section_length);
            continue;
        }
        if (!selected) continue;
        const uint8_t *equals = memchr(line.bytes, '=', line.length);
        if (!equals) continue;
        const SkseWinIniValue name = trim_ini((SkseWinIniValue){line.bytes, (size_t)(equals - line.bytes)});
        if (!ini_name_equal(name, key, key_length)) continue;
        SkseWinIniValue value = trim_ini((SkseWinIniValue){equals + 1, (size_t)(line.bytes + line.length - equals - 1)});
        if (value.length >= 2 && (*value.bytes == '\'' || *value.bytes == '"') &&
            *value.bytes == value.bytes[value.length - 1]) {
            ++value.bytes;
            value.length -= 2;
        }
        return value;
    }
    return missing;
}

uint32_t skse_win_ini_integer(SkseWinIniValue value) {
    value = trim_ini(value);
    const uint8_t *p = value.bytes;
    size_t left = value.length;
    const bool negative = left && *p == '-';
    if (left && (negative || *p == '+')) { ++p; --left; }
    unsigned base = 10;
    if (left >= 2 && p[0] == '0') {
        if (p[1] == 'x') base = 16;
        if (p[1] == 'o') base = 8;
        if (p[1] == 'b') base = 2;
        if (base != 10) { p += 2; left -= 2; }
    }
    uint32_t number = 0;
    while (left--) {
        const unsigned c = skse_win_ascii_lower(*p++);
        const unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
        if (digit >= base) break;
        number = number * base + digit;
    }
    return negative ? 0u - number : number;
}

static uint32_t read_asset(SkseWinFiles *files, const SkseWinAssets *source, const char *path, SkseWinFileData *data) {
    if (!source->read) return ACCESS_DENIED;
    size_t limit = MAX_OPENED_BYTES - files->opened_bytes;
    if (limit > SKSE_WIN_FILE_MAX_BYTES) limit = SKSE_WIN_FILE_MAX_BYTES;
    uint8_t *bytes = NULL;
    size_t length = 0;
    uint32_t status = source->read(source->context, path, limit, &bytes, &length);
    if (!status && length > limit) status = FILE_TOO_LARGE;
    if (!status && length && !bytes) status = INVALID_DATA;
    if (!status) {
        char *copy = copy_path(path);
        if (copy) {
            *data = (SkseWinFileData){.bytes = bytes, .path = copy, .length = length, .capacity = length};
            files->opened_bytes += length;
            return 0;
        }
        status = NOT_ENOUGH_MEMORY;
    }
    free(bytes);
    return status;
}

static SkseWinFileData *find_output(SkseWinFiles *files, const char *path) {
    for (unsigned i = 0; i < SKSE_WIN_OUTPUT_FILES; ++i) {
        SkseWinFileData *data = &files->outputs[i];
        if (data->path && skse_win_asset_name_equal(data->path, path)) return data;
    }
    return NULL;
}

bool skse_win_output_exists(SkseWinFiles *files, const char *path) {
    return find_output(files, path) != NULL;
}

static const char *child_path(const char *entry, const char *path) {
    while (*entry && skse_win_ascii_lower((uint8_t)*entry) == skse_win_ascii_lower((uint8_t)*path)) {
        ++entry;
        ++path;
    }
    return !*entry && *path == '/' ? path + 1 : NULL;
}

bool skse_win_local_attributes(SkseWinFiles *files, const char *path,
                                uint32_t *attributes, uint32_t *error) {
    if (find_output(files, path)) {
        *attributes = SKSE_WIN_ATTRIBUTE_NORMAL;
        *error = 0;
        return true;
    }

    uint32_t missing = 0;
    for (unsigned i = 0; i < files->directory_count; ++i) {
        const char *directory = files->directories[i];
        if (skse_win_asset_name_equal(directory, path)) {
            *attributes = SKSE_WIN_ATTRIBUTE_DIRECTORY;
            *error = 0;
            return true;
        }
        const char *child = child_path(directory, path);
        if (child && missing != FILE_NOT_FOUND)
            missing = strchr(child, '/') ? PATH_NOT_FOUND : FILE_NOT_FOUND;
    }
    for (unsigned i = 0; i < SKSE_WIN_OUTPUT_FILES; ++i) {
        const char *file = files->outputs[i].path;
        if (file && child_path(file, path)) {
            missing = PATH_NOT_FOUND;
            break;
        }
    }
    if (!missing) return false;
    *attributes = 0;
    *error = missing;
    return true;
}

static bool missing_path(uint32_t status) {
    return status == FILE_NOT_FOUND || status == PATH_NOT_FOUND;
}

static bool profile_path(const SkseWinFiles *files, const char *path) {
    return files->profile.allow_write && files->profile.allow_write(files->profile.input.context, path);
}

/* A profile directory cannot make an immutable file into its parent, even
 * when leaf metadata reports the ordinary Win32 path-not-found result. */
static uint32_t base_parent_status(const SkseWinAssets *base, const char *path) {
    if (!base->attributes) return 0;
    char parent[SKSE_WIN_PATH_BYTES];
    for (const char *slash = path; (slash = strchr(slash, '/')); ++slash) {
        size_t length = (size_t)(slash - path);
        memcpy(parent, path, length);
        parent[length] = 0;
        uint32_t attributes = 0;
        uint32_t result = base->attributes(base->context, parent, &attributes);
        if (!result && !(attributes & SKSE_WIN_ATTRIBUTE_DIRECTORY)) return PATH_NOT_FOUND;
        if (result && !missing_path(result)) return result;
    }
    return 0;
}

static uint32_t provider_attributes(const SkseWinFiles *files, const char *path, uint32_t *attributes) {
    uint32_t status = FILE_NOT_FOUND;
    if (files->source.attributes) {
        status = files->source.attributes(files->source.context, path, attributes);
        if (!missing_path(status)) return status;
    }
    if (!files->profile.allow_write || !files->profile.input.attributes) return status;
    if (files->source.read && !files->source.attributes) return ACCESS_DENIED;
    status = files->profile.input.attributes(files->profile.input.context, path, attributes);
    if (!status) {
        status = base_parent_status(&files->source, path);
        if (status) *attributes = 0;
    }
    return status;
}

bool skse_win_file_attributes(SkseWinFiles *files, const char *path, uint32_t *attributes, uint32_t *error) {
    if (skse_win_local_attributes(files, path, attributes, error)) return true;
    *attributes = 0;
    if (!files->source.attributes && !(files->profile.allow_write && files->profile.input.attributes)) {
        *error = ACCESS_DENIED;
        return true;
    }
    if (files->attribute_requests >= SKSE_WIN_ATTRIBUTE_REQUEST_LIMIT) return false;
    ++files->attribute_requests;
    *error = provider_attributes(files, path, attributes);
    return true;
}

static uint32_t lookup_attributes(SkseWinFiles *files, const char *path, uint32_t *attributes) {
    uint32_t status;
    if (skse_win_local_attributes(files, path, attributes, &status)) return status;
    return provider_attributes(files, path, attributes);
}

static uint32_t read_virtual_file(SkseWinFiles *files, const char *path, bool granted, SkseWinFile *slot) {
    const SkseWinAssets *base = &files->source;
    const SkseWinAssets *profile = &files->profile.input;
    if (!granted || !profile->read || !profile->attributes) return read_asset(files, base, path, &slot->snapshot);
    if (base->attributes) {
        uint32_t attributes = 0;
        uint32_t status = base->attributes(base->context, path, &attributes);
        if (!status) return read_asset(files, base, path, &slot->snapshot);
        if (!missing_path(status)) return status;
    } else if (base->read) {
        uint32_t status = read_asset(files, base, path, &slot->snapshot);
        if (!missing_path(status)) return status;
    }
    uint32_t status = base_parent_status(base, path);
    if (status) return status;
    status = read_asset(files, profile, path, &slot->snapshot);
    if (!status) slot->profile_snapshot = true;
    return status;
}

static uint32_t parent_directory_status(SkseWinFiles *files, const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return 0; // The virtual root always exists.
    char parent[SKSE_WIN_PATH_BYTES];
    size_t length = (size_t)(slash - path);
    memcpy(parent, path, length);
    parent[length] = '\0';
    uint32_t attributes = 0;
    uint32_t status = lookup_attributes(files, parent, &attributes);
    if (!status) return (attributes & SKSE_WIN_ATTRIBUTE_DIRECTORY) ? 0 : PATH_NOT_FOUND;
    return missing_path(status) ? PATH_NOT_FOUND : status;
}

uint32_t skse_win_directory_create(SkseWinFiles *files, const char *path) {
    if (files->read_only) return ACCESS_DENIED;
    if (files->directory_requests >= MAX_DIRECTORY_ATTEMPTS) return TOO_MANY_OPEN_FILES;
    ++files->directory_requests;
    // A read-only source without metadata cannot safely rule out collisions.
    if (files->source.read && !files->source.attributes) return ACCESS_DENIED;

    uint32_t attributes = 0;
    uint32_t status = lookup_attributes(files, path, &attributes);
    if (!status) return ALREADY_EXISTS;
    if (!missing_path(status)) return status;
    status = parent_directory_status(files, path);
    if (status) return status;

    if (files->directory_count >= SKSE_WIN_OUTPUT_DIRECTORIES) return TOO_MANY_OPEN_FILES;
    char *copy = copy_path(path);
    if (!copy) return NOT_ENOUGH_MEMORY;
    files->directories[files->directory_count++] = copy;
    return 0;
}

static SkseWinFileData *unused_output(SkseWinFiles *files) {
    for (unsigned i = 0; i < SKSE_WIN_OUTPUT_FILES; ++i)
        if (!files->outputs[i].path) return &files->outputs[i];
    return NULL;
}

static uint32_t create_output(SkseWinFiles *files, const char *path,
                              SkseWinFileDisposition disposition, SkseWinFileData **out) {
    uint32_t attributes = 0;
    uint32_t status = lookup_attributes(files, path, &attributes);
    if (!status) return disposition == SKSE_WIN_CREATE_NEW ? FILE_EXISTS : ACCESS_DENIED;
    if (!missing_path(status)) return status;
    if (disposition == SKSE_WIN_OPEN_EXISTING || disposition == SKSE_WIN_TRUNCATE_EXISTING)
        return FILE_NOT_FOUND;
    if (parent_directory_status(files, path)) return PATH_NOT_FOUND;
    SkseWinFileData *data = unused_output(files);
    if (!data) return TOO_MANY_OPEN_FILES;
    data->path = copy_path(path);
    if (!data->path) return NOT_ENOUGH_MEMORY;
    *out = data;
    return 0;
}

static uint32_t check_base_absent(const SkseWinFiles *files, const char *path, SkseWinFileDisposition disposition) {
    const SkseWinAssets *base = &files->source;
    if (!base->attributes) return base->read ? ACCESS_DENIED : 0;
    uint32_t attributes = 0;
    uint32_t status = base->attributes(base->context, path, &attributes);
    if (!status) return disposition == SKSE_WIN_CREATE_NEW ? FILE_EXISTS : ACCESS_DENIED;
    return missing_path(status) ? base_parent_status(base, path) : status;
}

static void destroy_data(SkseWinFileData *data);

static SkseWinFile *profile_reader(SkseWinFiles *files, const char *path) {
    for (unsigned i = 0; i < SKSE_WIN_OPEN_FILES; ++i) {
        SkseWinFile *slot = &files->files[i];
        if (slot->profile_snapshot && skse_win_asset_name_equal(slot->snapshot.path, path)) return slot;
    }
    return NULL;
}

static void promote_profile_reader(SkseWinFile *slot, SkseWinFileData *output) {
    destroy_data(&slot->snapshot);
    slot->data = output;
    slot->profile_snapshot = false;
}

static uint32_t create_profile_output(SkseWinFiles *files, const char *path, SkseWinFileDisposition disposition,
                                      SkseWinFileData **out, bool *existed) {
    const SkseWinAssets *profile = &files->profile.input;
    if (!profile->attributes || !profile->read) return ACCESS_DENIED;
    uint32_t status = check_base_absent(files, path, disposition);
    if (status) return status;
    uint32_t attributes = 0;
    status = profile->attributes(profile->context, path, &attributes);
    if (status && !missing_path(status)) return status;
    *existed = !status;
    if (*existed && (attributes & SKSE_WIN_ATTRIBUTE_DIRECTORY)) return ACCESS_DENIED;
    if (*existed && disposition == SKSE_WIN_CREATE_NEW) return FILE_EXISTS;
    if (!*existed && (disposition == SKSE_WIN_OPEN_EXISTING || disposition == SKSE_WIN_TRUNCATE_EXISTING))
        return FILE_NOT_FOUND;
    status = parent_directory_status(files, path);
    if (status) return status;
    SkseWinFileData *data = unused_output(files);
    if (!data) return TOO_MANY_OPEN_FILES;

    bool truncate = disposition == SKSE_WIN_CREATE_ALWAYS || disposition == SKSE_WIN_TRUNCATE_EXISTING;
    if (*existed && !truncate) {
        SkseWinFile *reader = profile_reader(files, path);
        if (reader) {
            // Move the already-open immutable snapshot into the COW slot.
            *data = reader->snapshot;
            reader->snapshot = (SkseWinFileData){0};
            promote_profile_reader(reader, data);
        } else {
            status = read_asset(files, profile, path, data);
            if (status) return status;
        }
    } else {
        data->path = copy_path(path);
        if (!data->path) return NOT_ENOUGH_MEMORY;
        data->profile_dirty = true;
    }
    data->profile = true;
    for (SkseWinFile *reader; (reader = profile_reader(files, path));)
        promote_profile_reader(reader, data);
    *out = data;
    return 0;
}

uint32_t skse_win_file_open(SkseWinFiles *files, const char *path, unsigned access, unsigned share,
                            SkseWinFileDisposition disposition, uint32_t *error) {
    if (files->read_only && ((access & SKSE_WIN_FILE_WRITE) || disposition != SKSE_WIN_OPEN_EXISTING)) {
        *error = ACCESS_DENIED;
        return UINT32_MAX;
    }
    SkseWinFileData *data = find_output(files, path);
    const bool granted = profile_path(files, path);
    uint32_t attributes, local_error;
    if (!data && skse_win_local_attributes(files, path, &attributes, &local_error) &&
        (!local_error || !(access & SKSE_WIN_FILE_WRITE))) {
        // Directory handles require FILE_FLAG_BACKUP_SEMANTICS, outside this VM.
        *error = local_error ? local_error : ACCESS_DENIED;
        return UINT32_MAX;
    }
    if (!data && !(access & SKSE_WIN_FILE_WRITE) && !files->source.read &&
        !(granted && files->profile.input.read)) {
        *error = ACCESS_DENIED;
        return UINT32_MAX;
    }
    SkseWinFile *slot = NULL;
    for (unsigned i = 0; i < SKSE_WIN_OPEN_FILES; ++i) {
        SkseWinFile *current = &files->files[i];
        if (current->token && ((access & ~current->share) || (current->access & ~share)) &&
            skse_win_asset_name_equal(current->data->path, path)) {
            *error = SHARING_VIOLATION;
            return UINT32_MAX;
        }
        if (!slot && !current->token) slot = current;
    }
    if (!slot || files->requests - files->request_window_start >= MAX_OPEN_ATTEMPTS ||
        files->requests >= UINT32_C(0x00ffffff)) {
        *error = TOO_MANY_OPEN_FILES;
        return UINT32_MAX;
    }
    ++files->requests;
    bool exists = data != NULL;
    uint32_t status = 0;
    if (exists) {
        if (data->profile && (access & SKSE_WIN_FILE_WRITE))
            status = granted ? check_base_absent(files, path, disposition) : ACCESS_DENIED;
        if (!status && disposition == SKSE_WIN_CREATE_NEW) status = FILE_EXISTS;
        if (!status && (disposition == SKSE_WIN_CREATE_ALWAYS || disposition == SKSE_WIN_TRUNCATE_EXISTING)) {
            // The legacy dispatcher never supplies a truncating disposition
            // without write access. Enforce it again for profile data.
            if (data->profile && !(access & SKSE_WIN_FILE_WRITE)) status = ACCESS_DENIED;
            else {
                data->length = 0;
                if (data->profile) data->profile_dirty = true;
            }
        }
    } else if (access & SKSE_WIN_FILE_WRITE) {
        status = granted ? create_profile_output(files, path, disposition, &data, &exists) :
            create_output(files, path, disposition, &data);
    } else {
        data = &slot->snapshot;
        status = read_virtual_file(files, path, granted, slot);
        if (!status && slot->profile_snapshot) exists = true;
    }
    if (status) {
        *error = status;
        return UINT32_MAX;
    }
    if (disposition == SKSE_WIN_CREATE_ALWAYS || disposition == SKSE_WIN_OPEN_ALWAYS)
        *error = exists ? ALREADY_EXISTS : 0;
    slot->token = UINT32_C(0xe3000000) + files->requests;
    slot->data = data;
    slot->access = access;
    slot->share = share;
    return slot->token;
}

SkseWinFile *skse_win_file_lookup(SkseWinFiles *files, uint32_t token) {
    if (!token) return NULL;
    for (unsigned i = 0; i < SKSE_WIN_OPEN_FILES; ++i)
        if (files->files[i].token == token) return &files->files[i];
    return NULL;
}

static uint32_t profile_write_error(SkseWinFileData *data, uint32_t error) {
    if (data->profile && !data->profile_error) data->profile_error = error;
    return error;
}

void skse_win_file_note_write(SkseWinFile *file, bool success) {
    if (!file->data->profile) return;
    if (success) file->data->profile_dirty = true;
    else profile_write_error(file->data, WRITE_FAULT);
}

uint32_t skse_win_file_resize(SkseWinFile *file, uint64_t length) {
    if (!(file->access & SKSE_WIN_FILE_WRITE)) return ACCESS_DENIED;
    if (length > SKSE_WIN_FILE_MAX_BYTES) return profile_write_error(file->data, FILE_TOO_LARGE);
    SkseWinFileData *data = file->data;
    size_t end = (size_t)length;
    if (end > data->capacity) {
        size_t capacity = data->capacity ? data->capacity : 64;
        while (capacity < end) capacity = capacity <= SKSE_WIN_FILE_MAX_BYTES / 2 ?
            capacity * 2 : SKSE_WIN_FILE_MAX_BYTES;
        uint8_t *bytes = realloc(data->bytes, capacity);
        if (!bytes) return profile_write_error(data, NOT_ENOUGH_MEMORY);
        data->bytes = bytes;
        data->capacity = capacity;
    }
    if (end > data->length) memset(data->bytes + data->length, 0, end - data->length);
    if (data->profile && end != data->length) data->profile_dirty = true;
    data->length = end;
    return 0;
}

static void destroy_data(SkseWinFileData *data) {
    free(data->bytes);
    free(data->path);
    *data = (SkseWinFileData){0};
}

void skse_win_file_close(SkseWinFile *file) {
    destroy_data(&file->snapshot);
    *file = (SkseWinFile){0};
}

void skse_win_files_close(SkseWinFiles *files) {
    for (unsigned i = 0; i < SKSE_WIN_OPEN_FILES; ++i) skse_win_file_close(&files->files[i]);
    for (unsigned i = 0; i < SKSE_WIN_OUTPUT_FILES; ++i) destroy_data(&files->outputs[i]);
    for (unsigned i = 0; i < files->directory_count; ++i) {
        free(files->directories[i]);
        files->directories[i] = NULL;
    }
    files->directory_count = 0;
}

uint32_t skse_win_profile_closed_outputs(SkseWinFiles *files, bool callback_succeeded, SkseWinProfileOutputs *out) {
    if (!out) return INVALID_PARAMETER;
    *out = (SkseWinProfileOutputs){0};
    if (!files) return INVALID_PARAMETER;
    if (!callback_succeeded) files->profile_aborted = true;
    if (files->profile_aborted) return OPERATION_ABORTED;

    SkseWinProfileOutputs ready = {0};
    for (unsigned i = 0; i < SKSE_WIN_OUTPUT_FILES; ++i) {
        const SkseWinFileData *data = &files->outputs[i];
        if (!data->path || !data->profile) continue;
        if (data->profile_error) return data->profile_error;
        if (!data->profile_dirty) continue;
        for (unsigned j = 0; j < SKSE_WIN_OPEN_FILES; ++j)
            if (files->files[j].token && files->files[j].data == data) return SHARING_VIOLATION;
        if (!profile_path(files, data->path)) return ACCESS_DENIED;
        uint32_t status = check_base_absent(files, data->path, SKSE_WIN_OPEN_EXISTING);
        if (status) return status;
        if (data->length > SKSE_WIN_FILE_MAX_BYTES || data->length > data->capacity ||
            (data->length && !data->bytes)) return INVALID_DATA;
        ready.entries[ready.count++] = data;
    }
    *out = ready;
    return 0;
}

bool skse_win_profile_release_closed(SkseWinFiles *files, const SkseWinFileData *published) {
    if (!files || !published || files->profile_aborted) return false;
    for (unsigned i = 0; i < SKSE_WIN_OUTPUT_FILES; ++i) {
        SkseWinFileData *data = &files->outputs[i];
        if (data != published || !data->path || !data->profile || !data->profile_dirty || data->profile_error)
            continue;
        for (unsigned j = 0; j < SKSE_WIN_OPEN_FILES; ++j)
            if (files->files[j].token && files->files[j].data == data) return false;
        destroy_data(data);
        return true;
    }
    return false;
}

bool skse_win_profile_begin_call(SkseWinFiles *files) {
    if (!files || !files->profile.allow_write || files->profile_aborted) return false;
    for (unsigned i = 0; i < SKSE_WIN_OPEN_FILES; ++i) {
        const SkseWinFile *file = &files->files[i];
        if (file->token && (file->profile_snapshot || (file->data && file->data->profile))) return false;
    }
    files->request_window_start = files->requests;
    files->opened_bytes = 0;
    files->attribute_requests = 0;
    files->directory_requests = 0;
    return true;
}

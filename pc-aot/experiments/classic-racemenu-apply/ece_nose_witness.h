/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct CPU;

typedef bool (*EceNoseRead)(void *context, uint32_t address, void *output, size_t size);
/* The hosting fixture owns source validation, independently of the selected
 * path. Begin scopes an already-open source and finish validates that receipt. */
typedef uint64_t (*EceNoseSourceBegin)(void *context, const struct CPU *cpu, uint32_t path, uint32_t stream);
typedef bool (*EceNoseSourceFinish)(void *context, const struct CPU *cpu, uint64_t receipt);

typedef struct {
    bool female_nord, head, geometry, selected_path, fresh_tri, morph_record;
    bool layout_checked, layout_complete;
} EceNoseWitnessReport;

typedef void (*EceNoseReport)(void *context, EceNoseWitnessReport report);

bool ece_nose_witness_arm(EceNoseRead read, EceNoseSourceBegin begin, EceNoseSourceFinish finish,
                         EceNoseReport report, void *context);
void ece_nose_witness_disarm(void);
bool ece_nose_menu(uint32_t caller, uint32_t menu);
void ece_nose_menu_end(bool active);
bool ece_nose_begin(uint32_t menu, uint32_t preview, uint32_t descriptor_race);
void ece_nose_end(bool active);
void ece_nose_face(uint32_t caller, uint32_t menu, uint32_t actor, uint32_t base, uint32_t face);
void ece_nose_parts(uint32_t base, uint32_t face, uint32_t list, uint32_t count);
bool ece_nose_head(uint32_t caller, uint32_t face, uint32_t part, uint32_t geometry);
void ece_nose_head_end(bool active);
bool ece_nose_lookup(uint32_t path);
void ece_nose_lookup_end(bool active, uint32_t result);
uint64_t ece_nose_parse(const struct CPU *cpu, uint32_t path, uint32_t stream);
bool ece_nose_parsed(const struct CPU *cpu, uint64_t receipt, bool parsed);
void ece_nose_constructed(bool fresh, uint32_t entry);
void ece_nose_retiring(uint32_t entry);
bool ece_nose_morph(uint32_t tri, uint32_t geometry);
void ece_nose_morph_record(bool active, uint32_t name, bool matched);

/* SPDX-License-Identifier: GPL-3.0-only */
/* Game and MOD code are never loaded. Both lookups receive these same objects
 * in a private Win32 test arena. Original routines come from the local SDK. */
#include "favorite.h"
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

using UInt8 = uint8_t;
using UInt32 = uint32_t;
using SInt32 = int32_t;
static_assert(sizeof(void *) == 4, "Run the Classic reference as an x86 process");

alignas(16) static unsigned char storage[32 * 1024];
static size_t used;

static void *FormHeap_Allocate(size_t size) {
    size_t start = (used + 15) & ~size_t(15);
    if (size > sizeof(storage) - start) std::abort();
    used = start + size;
    std::memset(storage + start, 0, size);
    return storage + start;
}
static void FormHeap_Free(void *) { std::abort(); }  // A lookup must never free fixture inputs.
#define ASSERT(x) assert(x)
template<class T> static T *make() { return new (FormHeap_Allocate(sizeof(T))) T{}; }
static uint32_t address(const void *p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }

struct TESForm {
    uint8_t prefix[12];
    uint32_t id;
    uint8_t padding[2];
    uint8_t formType;
};
struct StaticFunctionTag {};
#include "sdk_collections.inc"

constexpr UInt32 kExtraData_ContainerChanges = 0x15, kExtraData_Hotkey = 0x49;
constexpr UInt8 kFormType_Spell = 22, kFormType_Shout = 119;
struct BSExtraData {
    virtual ~BSExtraData() = default;
    BSExtraData *next{};
};
struct BaseExtraList {
    #include "sdk_presence.inc"
    BSExtraData *GetByType(UInt32 type) const;
    BSExtraData *m_data{};
    PresenceBitfield *m_presence{};
};
using ExtendDataList = tList<BaseExtraList>;
struct ExtraHotkey : BSExtraData { int8_t hotkey{}; };
struct InventoryEntryData {
    TESForm *type{};
    ExtendDataList *extendDataList{};
    int32_t countDelta{};
};
struct ExtraContainerChanges : BSExtraData {
    struct Data { tList<InventoryEntryData> *objList{}; };
    struct FoundHotkeyData { TESForm *pForm; ExtraHotkey *pHotkey; };
    Data *data{};
    FoundHotkeyData FindHotkey(TESForm *) const;
};
using HotkeyData = ExtraContainerChanges::FoundHotkeyData;
struct PlayerCharacter { uint8_t padding[0x48]; BaseExtraList extraData; };
struct MagicFavorites {
    uint32_t padding[2];
    tArray<TESForm *> spells, hotkeys;
    static MagicFavorites *active;
    static MagicFavorites *GetSingleton() { return active; }
    bool IsFavorited(TESForm *);
};
MagicFavorites *MagicFavorites::active;
static PlayerCharacter *player;
static PlayerCharacter **g_thePlayer = &player;

// Only the engine RTTI service is substituted. Use actual MSVC RTTI on our
// own derived types so presence bits alone cannot create a positive answer.
static const UInt8 types[] = {0, 1, 2};
static const void *RTTI_BSExtraData = types;
static const void *RTTIForExtraType[0xb4]{};
static void *Runtime_DynamicCast(void *object, int, const void *, const void *target, int) {
    auto *extra = static_cast<BSExtraData *>(object);
    if (target == &types[1]) return dynamic_cast<ExtraContainerChanges *>(extra);
    if (target == &types[2]) return dynamic_cast<ExtraHotkey *>(extra);
    return nullptr;
}
#include "sdk_favorites.inc"

static_assert(offsetof(TESForm, formType) == 0x12);
static_assert(sizeof(tList<TESForm>) == 8 && sizeof(tArray<TESForm *>) == 12);
static_assert(sizeof(BSExtraData) == 8 && sizeof(BaseExtraList) == 8);
static_assert(offsetof(PlayerCharacter, extraData) == 0x48);
static_assert(offsetof(MagicFavorites, spells) == 8 && offsetof(MagicFavorites, hotkeys) == 0x14);

static bool contains(uint32_t location, size_t size) {
    return location >= address(storage) && size <= used && location - address(storage) <= used - size;
}
static bool read_bytes(void *, uint32_t location, void *out, size_t size) {
    if (!contains(location, size)) return false;
    std::memcpy(out, reinterpret_cast<const void *>(location), size);
    return true;
}
static bool find_extra(void *, uint32_t location, uint32_t type, uint32_t *out) {
    if (!contains(location, sizeof(BaseExtraList)) ||
        (type != kExtraData_ContainerChanges && type != kExtraData_Hotkey)) return false;
    const auto *list = reinterpret_cast<const BaseExtraList *>(location);
    *out = address(list->GetByType(type));
    return true;
}
static const FavoriteMemory memory = {nullptr, read_bytes, find_extra};

static void add_extra(BaseExtraList &list, UInt32 type, BSExtraData *extra, bool mark = true) {
    if (mark) {
        if (!list.m_presence) list.m_presence = make<BaseExtraList::PresenceBitfield>();
        list.m_presence->bits[type / 8] |= static_cast<UInt8>(1u << (type % 8));
    }
    if (extra) {
        extra->next = list.m_data;
        list.m_data = extra;
    }
}

struct World {
    enum { N = 8 };
    struct Forms { TESForm values[N]; };
    Forms *forms;
    ExtraContainerChanges *changes;
    PlayerCharacter *owner;
    MagicFavorites *magic;
    tList<InventoryEntryData> *entries;

    World() {
        used = 0;
        forms = make<Forms>();
        const uint8_t form_types[N] = {22, 41, 119, 26, 22, 119, 41, 32};
        for (unsigned i = 0; i < N; ++i) {
            form(i)->formType = form_types[i];
            form(i)->id = 2048;  // All distinct pointers deliberately have the same numeric ID.
        }
        player = owner = make<PlayerCharacter>();
        MagicFavorites::active = magic = make<MagicFavorites>();
        changes = make<ExtraContainerChanges>();
        changes->data = make<ExtraContainerChanges::Data>();
        changes->data->objList = entries = make<tList<InventoryEntryData>>();
        add_extra(owner->extraData, kExtraData_ContainerChanges, changes);
        if (address(&changes->data) - address(changes) != 8) std::abort();
    }
    TESForm *form(unsigned i) const { return &forms->values[i]; }

    void fill(tArray<TESForm *> &array, unsigned mask) {
        array.arr.entries = static_cast<TESForm **>(FormHeap_Allocate(sizeof(TESForm *) * N));
        array.arr.capacity = N;
        array.count = 0;
        for (unsigned i = 0; i < N; ++i)
            if ((mask >> i) & 1u) array.arr.entries[array.count++] = form(i);
    }
    InventoryEntryData *entry(unsigned item) {
        auto *entry = make<InventoryEntryData>();
        entry->type = form(item);
        entry->extendDataList = make<ExtendDataList>();
        entries->Push(entry);
        return entry;
    }
    static BaseExtraList *stack(InventoryEntryData *entry, int hotkey = -99) {
        auto *list = make<BaseExtraList>();
        if (hotkey != -99) {
            auto *key = make<ExtraHotkey>();
            key->hotkey = static_cast<int8_t>(hotkey);
            add_extra(*list, kExtraData_Hotkey, key);
        }
        entry->extendDataList->Push(list);
        return list;
    }
};

static unsigned matches, positives, invalid_cases;
static uint32_t digest = 2166136261u;

static void check(const char *label, TESForm *form, bool expected) {
    bool original = papyrusGame::IsObjectFavorited(nullptr, form);
    auto result = classic_favorite_query(&memory, address(player), address(MagicFavorites::active), address(form));
    if (result != static_cast<int>(expected) || original != expected) {
        std::fprintf(stderr, "%s [%u]: expected %d, Classic %d, candidate %d\n",
                     label, matches, expected, original, result);
        std::exit(1);
    }
    ++matches;
    positives += original;
    digest = (digest ^ static_cast<unsigned>(original)) * 16777619u;
}

template<class T> static void hole_at_head(tList<T> *list) {
    T *empty = nullptr;
    std::memcpy(static_cast<void *>(list), &empty, sizeof(empty));
}

static void named_cases() {
    World world;
    check("no form", nullptr, false);
    world.fill(world.magic->spells, 1u << 0 | 1u << 2);
    world.fill(world.magic->hotkeys, 1u << 4);
    check("spell without assigned hotkey", world.form(0), true);
    check("shout present", world.form(2), true);
    check("numbered slot alone", world.form(4), false);
    MagicFavorites::active = nullptr;
    check("no magic singleton", world.form(2), false);
    MagicFavorites::active = world.magic;
    player = nullptr;
    check("no player even if magic is present", world.form(0), false);
    player = world.owner;
    check("empty inventory", world.form(1), false);

    auto *other = world.entry(6);
    world.stack(other, 1);
    check("other copy with same form ID", world.form(1), false);
    auto *item = world.entry(1);
    auto *empty = world.stack(item);
    add_extra(*empty, kExtraData_Hotkey, make<BSExtraData>());
    check("presence bit with no matching extra type", world.form(1), false);
    auto *own_key = make<ExtraHotkey>();
    add_extra(*world.stack(item), kExtraData_Hotkey, own_key, false);
    check("object without presence bit", world.form(1), false);
    item->countDelta = -1;
    world.stack(item, -1);
    check("favorited without a number", world.form(1), true);
    check("numbered inventory hotkey", world.form(6), true);
    world.stack(world.entry(4), 1);
    check("inventory entry cannot favorite a spell", world.form(4), false);
    world.fill(world.magic->spells, 1u << 3);
    check("magic list cannot favorite ordinary item", world.form(3), false);
    auto *missing = world.entry(7);
    missing->extendDataList = nullptr;
    check("no instance list", world.form(7), false);
    world.stack(world.entry(7), 0);
    check("same form on a later inventory entry", world.form(7), true);
    hole_at_head(item->extendDataList);
    check("inner list ends on a null entry", world.form(1), false);
    hole_at_head(world.entries);
    check("outer inventory visit passes a null entry", world.form(7), true);
    player->extraData.m_presence = nullptr;
    check("no container changes", world.form(7), false);
}

static uint32_t random_word(uint32_t &state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static void varied_cases() {
    uint32_t rng = 0x511073;
    for (unsigned i = 0; i < 2048; ++i) {
        World w;
        uint32_t known = random_word(rng);
        w.fill(w.magic->spells, known);
        w.fill(w.magic->hotkeys, random_word(rng));
        bool inventory[World::N]{};
        unsigned length = random_word(rng) % 13;
        for (unsigned n = 0; n < length; ++n) {
            unsigned item = random_word(rng) % World::N;
            auto *entry = w.entry(item);
            entry->countDelta = static_cast<int32_t>(random_word(rng) % 11) - 5;
            unsigned count = random_word(rng) % 5;
            for (unsigned j = 0; j < count; ++j) {
                auto *stack = w.stack(entry);
                switch (random_word(rng) % 6) {
                case 0: case 1: {
                    auto *key = make<ExtraHotkey>();
                    key->hotkey = static_cast<int8_t>(random_word(rng) % 10) - 1;
                    add_extra(*stack, kExtraData_Hotkey, key);
                    inventory[item] = true;
                    break;
                }
                case 2: add_extra(*stack, kExtraData_Hotkey, make<BSExtraData>()); break;
                case 3: add_extra(*stack, kExtraData_Hotkey, make<ExtraHotkey>(), false); break;
                }
            }
        }
        if (i % 7 == 0) MagicFavorites::active = nullptr;
        if (i % 13 == 0) player = nullptr;
        for (unsigned item = 0; item < World::N; ++item) {
            TESForm *form = w.form(item);
            bool spell = form->formType == kFormType_Spell || form->formType == kFormType_Shout;
            bool expected = spell ? MagicFavorites::active && ((known >> item) & 1u) : inventory[item];
            check("combined world", form, player && expected);
        }
    }
}

static void candidate_only(const char *label, const World &w, unsigned item, FavoriteResult expected) {
    auto got = classic_favorite_query(&memory, address(w.owner), address(w.magic), address(w.form(item)));
    if (got != expected) {
        std::fprintf(stderr, "%s: expected %d, got %d\n", label, expected, got);
        std::exit(1);
    }
    ++invalid_cases;
}
static void incomplete_and_bad_inputs() {
    World w;
    auto *entry = w.entry(1);
    auto *saved = w.changes->data;
    w.changes->data = nullptr;
    // Classic's helper leaves its two output pointers uninitialized in these
    // two partial states. Its result is deliberately never used as an oracle.
    candidate_only("missing container data", w, 1, FAVORITE_NO);
    w.changes->data = saved;
    w.changes->data->objList = nullptr;
    candidate_only("missing inventory list", w, 1, FAVORITE_NO);
    w.changes->data->objList = w.entries;
    w.fill(w.magic->spells, 1);
    w.magic->spells.count = 9;
    candidate_only("count exceeds capacity", w, 0, FAVORITE_INVALID);
    w.magic->spells.count = 2;
    w.magic->spells.arr.entries = reinterpret_cast<TESForm **>(UINT32_C(0xfffffffc));
    candidate_only("array overflows guest address space", w, 0, FAVORITE_INVALID);
    w.magic->spells.arr.entries = reinterpret_cast<TESForm **>(4);
    candidate_only("unreadable array", w, 0, FAVORITE_INVALID);
    uint32_t head = address(w.entries);
    std::memcpy(reinterpret_cast<unsigned char *>(w.entries) + 4, &head, 4);
    candidate_only("inventory cycle", w, 7, FAVORITE_INVALID);
    w.stack(entry);
    head = address(entry->extendDataList);
    std::memcpy(reinterpret_cast<unsigned char *>(entry->extendDataList) + 4, &head, 4);
    candidate_only("instance cycle", w, 1, FAVORITE_INVALID);
    entry->extendDataList = reinterpret_cast<ExtendDataList *>(UINT32_C(0xfffffff8));
    candidate_only("unreadable instance list", w, 1, FAVORITE_INVALID);
}

int main() {
    RTTIForExtraType[kExtraData_ContainerChanges] = &types[1];
    RTTIForExtraType[kExtraData_Hotkey] = &types[2];
    named_cases();
    varied_cases();
    incomplete_and_bad_inputs();
    std::printf("favorites comparisons=%u yes=%u no=%u output_fnv1a=%08x candidate_only=%u\n",
                matches, positives, matches - positives, digest, invalid_cases);
}

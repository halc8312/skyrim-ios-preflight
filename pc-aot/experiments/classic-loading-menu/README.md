# Classic Loading Menu transition probe

Safety Load LE 1.2 uses the Classic menu-manager singleton at 0x012E3548 and registers an event sink with the dispatcher at offset +4. The experiment observes the two original menu-open and menu-close dispatch calls in the pinned Classic LE 1.9.32 lift. It checks the authentic dispatcher, the original event's Boolean and the interned name slots in the pinned UIStringHolder. The original dispatch always runs. [Classic SKSE 1.7.3 source](https://skse.silverlock.org/)

The two sites are 0x00A5D6BC (close) and 0x00A5D9CF (open) in f_A5D370. The stager checks the whole unit and each call anchor. The hosted test compiles the staged unit and exercises the two original call sequences with an authored dispatcher and guest memory. It compares register, flag and stack results, verifies the guest event fields stay intact, and rejects another dispatcher, missing objects, unreadable events and unknown menus. A separate test exercises menu nesting and touch eligibility with authored events.

Run from this directory with a completed locally owned original-body generation:

```sh
SKYRIM_TEST_OWNED_CLASSIC_LIFT=/path/to/generation \
  python3 -B -m unittest -v test_loading_menu
```

Set SKYRIM_TEST_SANITIZERS=1 to add ASan and UBSan. The test generates C in a temporary directory and checks the app observer's output against the original two dispatch calls.

The pointer fixture uses the original executable identified in `generation.json` for its two menu switch tables and three vtables. It sends a button-free hover through the original cursor functions, captures the queued message, and runs the generic message-routing block with an authored open-menu stack. The original Inventory and Magic handlers deliver the mapped coordinates to their respective movie views. A modal that consumes the message blocks delivery to the list underneath. If the stack changes after enqueueing, the same message can instead reach the newly open menu. The original input tick also calls the input dispatcher with an empty event list. The test stops at the movie's event entry point. Visible row selection and iPhone game-thread integration remain unverified.

Include `loading_menu_observe` in `build_title_guest.py --mod-hooks` to stage this observer in a private guest. The Classic preparation also renames an unrelated entry in the same unit. The composition verifies that exact rename before applying both changes. On a verified LE build outside the no-MOD recovery path, the host also tracks named menu events for the native touch overlay. Six list menus enable directional swipe input. Nested confirmation and quantity dialogs suspend that input. Detailed diagnostics record loading and character-creation openings, closings and monotonic timestamps. The optional hook table accepts the original version 1 prefix for older guests and hosts.

This gives the iPhone diagnostics markers for the loading-screen and character-creation intervals. These are also the two gates used by Safety Load 1.2. Event delivery on iPhone and the allocator fallback still require verification. [Safety Load author's description](https://www.nexusmods.com/skyrim/mods/46465)

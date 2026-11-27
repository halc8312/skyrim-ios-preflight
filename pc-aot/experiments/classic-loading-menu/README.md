# Classic Loading Menu transition probe

Safety Load LE 1.2 uses the Classic menu-manager singleton at 0x012E3548 and registers an event sink with the dispatcher at offset +4. The experiment observes the two original menu-open and menu-close dispatch calls in the pinned Classic LE 1.9.32 lift. It checks the authentic dispatcher, the original event's Boolean and the interned name slots in the pinned UIStringHolder. The original dispatch always runs. [Classic SKSE 1.7.3 source](https://skse.silverlock.org/)

The two sites are 0x00A5D6BC (close) and 0x00A5D9CF (open) in f_A5D370. The stager checks the whole unit and each call anchor. The hosted test compiles the staged unit and exercises the two original call sequences with an authored dispatcher and guest memory. It compares register, flag and stack results, verifies the guest event fields stay intact, and rejects another dispatcher, missing objects, unreadable events and unknown menus. A separate test exercises menu nesting and touch eligibility with authored events.

Run from this directory with a completed locally owned original-body generation:

```sh
SKYRIM_TEST_OWNED_CLASSIC_LIFT=/path/to/generation \
  python3 -B -m unittest -v test_loading_menu
```

Set SKYRIM_TEST_SANITIZERS=1 to add ASan and UBSan. The test generates C in a temporary directory and checks the app observer's output against the original two dispatch calls.

The pointer fixture reads the two original menu switch tables and five menu vtables from the owner's executable identified in `generation.json`. It sends a button-free pointer move through the original cursor functions, captures the queued message, and runs the original generic message route with an authored open-menu stack. Fader and Mist let the move pass to the list. A modal that consumes the move blocks delivery. If Inventory closes and Magic opens after enqueueing, the same message can reach Magic. The original input tick also dispatches with an empty hardware event list.

On a rebuilt guest with `loading_menu_observe`, the optional touch setting enables a tap to highlight an item in Inventory or Magic. It defaults to off and takes effect after an app restart. The host maps touches inside the displayed game viewport. The live menu's original `IMenu::AdvanceMovie` instance sends one button-free move directly to its own movie through its original message handler. A menu change, a nested prompt, backgrounding, or a request older than 250 ms invalidates the pending tap. In the fixture, both original handlers deliver the expected coordinates, and the original guest call restores the caller's registers, flags and stack. Visible selection with each in-game list or SkyUI layout and the iPhone's game-thread behavior still need device testing.

Include `loading_menu_observe` in `build_title_guest.py --mod-hooks` to stage this observer in a private guest. The Classic preparation also renames an unrelated entry in the same unit. The composition verifies that exact rename before applying both changes. On a verified LE build outside the no-MOD recovery path, the host also tracks named menu events for the native touch overlay. Six list menus enable directional swipe input. Nested confirmation and quantity dialogs suspend that input. Detailed diagnostics record loading and character-creation openings, closings and monotonic timestamps. The optional hook table accepts the original version 1 prefix for older guests and hosts.

This gives the iPhone diagnostics markers for the loading-screen and character-creation intervals. These are also the two gates used by Safety Load 1.2. Event delivery on iPhone and the allocator fallback still require verification. [Safety Load author's description](https://www.nexusmods.com/skyrim/mods/46465)

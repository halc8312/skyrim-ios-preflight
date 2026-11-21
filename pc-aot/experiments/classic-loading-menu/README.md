# Classic Loading Menu transition probe

Safety Load LE 1.2 uses the Classic menu-manager singleton at 0x012E3548 and registers an event sink with the dispatcher at offset +4. The experiment observes the two original menu-open and menu-close dispatch calls in the pinned Classic LE 1.9.32 lift. It checks the authentic dispatcher, the pinned Loading Menu name slot and the original event's Boolean. No DLL is loaded and the original dispatch always runs. [Classic SKSE 1.7.3 source](https://skse.silverlock.org/)

The two sites are 0x00A5D6BC (close) and 0x00A5D9CF (open) in f_A5D370. The stager checks the whole unit and each call anchor. The hosted test compiles the staged unit and exercises the two original call sequences with an authored dispatcher and guest memory. It compares register, flag and stack results, verifies the guest event fields stay intact, and rejects another dispatcher, missing objects, unreadable events and another menu.

Run from this directory with a completed locally owned original-body generation:

```sh
SKYRIM_TEST_OWNED_CLASSIC_LIFT=/path/to/generation \
  python3 -B -m unittest -v test_loading_menu
```

Set SKYRIM_TEST_SANITIZERS=1 to add ASan and UBSan. The test generates C in a temporary directory and checks the app observer's output against the original two dispatch calls.

Include `loading_menu_observe` in `build_title_guest.py --mod-hooks` to stage this observer in a private guest. The Classic preparation also renames an unrelated entry in the same unit. The composition verifies that exact rename before applying both changes. The host selects this group only for a verified LE build with detailed diagnostics enabled, outside the no-MOD recovery path. It records opening, closing and monotonic timestamps without logging guest pointers or paths. The optional hook table accepts the original version 1 prefix for older guests and hosts.

This gives the iPhone diagnostics a marker for the loading-screen interval. Safety Load 1.2's allocation fallback and iPhone event delivery still require verification. [Safety Load author's description](https://www.nexusmods.com/skyrim/mods/46465)

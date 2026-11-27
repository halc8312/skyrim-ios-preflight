# Classic inventory movie fit

The fixed Classic loader passes the requested scale mode to MovieView, reads its visible frame, sets a safe rectangle from the game settings, and passes the original viewport dimensions to MovieView. With the 1280×720 Inventory and Magic stages and a 1564×720 viewport, a centered NoBorder fit would crop about 65 stage units at the top and bottom. The game viewport and the iOS controls can both affect what the user can see. [Classic SKSE 1.7.3 source archive](https://skse.silverlock.org/) · [Scaleform view scale modes](https://help.autodesk.com/cloudhelp/ENU/ScaleformStudio-Help/lua_ref/members.html)

This stage replaces NoBorder with ShowAll on wider displays for the reviewed Barter, Book, Container, Crafting, Dialogue, Gift, Inventory, Magic, RaceSex and Training menus. The threshold allows for integer rounding. The group can be combined with `loading_menu_observe` and is on by default in a normal launch when available in the guest. The MOD details setting takes effect after restart. [CommonLib Scaleform interface](https://parapets.commonlib.dev/GFxMovieView_8h_source.html)

The hosted test executes the staged call sequence and the original viewport setter with an authored MovieView. It checks the order of the scale, visible frame, safe area and viewport calls, the 16:9 and wide paths, and the original dimensions arriving at the viewport setter. The scale-only cases also cover unrelated menus, other scale modes, invalid dimensions and the disabled path. The MovieView behavior, SWF layout and touch result still need an iPhone comparison.

```sh
SKYRIM_TEST_OWNED_CLASSIC_LIFT=/path/to/generation \
  python3 -B -m unittest -v test_inventory_fit
```

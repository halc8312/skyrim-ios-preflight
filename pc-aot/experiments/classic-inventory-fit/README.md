# Classic inventory movie fit

The fixed Classic loader passes the requested scale mode to MovieView, reads its visible frame, sets a safe rectangle from the game settings, and passes the original viewport dimensions to MovieView. With the 1280×720 Inventory and Magic stages and a 1564×720 viewport, a centered NoBorder fit would crop about 65 stage units at the top and bottom. The game viewport and the iOS controls can both affect what the user can see. [Classic SKSE 1.7.3 source archive](https://skse.silverlock.org/) · [Scaleform view scale modes](https://help.autodesk.com/cloudhelp/ENU/ScaleformStudio-Help/lua_ref/members.html)

This stage replaces NoBorder with ShowAll on wider displays for the reviewed Barter, Book, Container, Crafting, Dialogue, Gift, Inventory, Magic, RaceSex and Training menus. The threshold allows for integer rounding. The group can be combined with `loading_menu_observe` and is on by default in a normal launch when available in the guest. The MOD details setting takes effect after restart. [CommonLib Scaleform interface](https://parapets.commonlib.dev/GFxMovieView_8h_source.html)

The hosted test runs the verified call slices for all ten menu constructors, the Classic loader through its viewport call and the original viewport setter. It supplies successful movie lookup and an authored MovieView. Each constructor is tested at 1280×720, 782×360, and 1564×720, with the inactive correction also checked at 1564×720. It verifies the original constructor arguments, the order of the Scaleform calls, the safe insets and the exact framebuffer dimensions reaching the viewport setter. The scale-only cases also cover unrelated menus, other scale modes and invalid dimensions. Actual Scaleform rendering and touch still need an iPhone comparison.

The English and Japanese stock stages for these ten menus, including the Update archive overrides, are 1280×720. An iPhone comparison beyond Inventory and Magic can use the name at the top of a crafting menu and the confirm or exit text below the cost in a training menu. The original crafting layout anchors the heading horizontally. The training text extends below the expected NoBorder range while the cost fits inside it. A book's page controls re-anchor to the lower left and can serve as a comparison. Character creation re-anchors its outer labels but can still crop the last row of the slider list.

When Detailed diagnostics is active at launch, `IOS_MENU_VIEWPORT` records the actual mode and alignment, buffer dimensions and viewport, visible frame and safe rectangle for each opened Inventory or Magic movie. The visible frame matches the Classic getter's conversion from twips to stage pixels. The viewport dimensions and coordinates come from the game framebuffer. The safe rectangle retains its original getter units. This sampling shares the existing menu callback and needs no guest rebuild. It keeps the row tap setting independent and only inspects the reviewed Classic MovieView on the menu's owner thread. The record contains no SWF paths, item text or guest addresses.

```sh
SKYRIM_TEST_OWNED_CLASSIC_LIFT=/path/to/generation \
  python3 -B -m unittest -v test_inventory_fit
```

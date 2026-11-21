# Classic inventory movie fit

The original inventory constructor at `f_869A70` passes scale mode 3 for `InventoryMenu` to the movie loader `f_A60C80`. The loader dispatches that integer to MovieView slot `0x1B`, then builds its viewport using the display's dimensions. The Scaleform definition of NoBorder explains why content can be clipped. This is evidence for a game-side crop, alongside any overlap from the iOS controls. [Classic SKSE 1.7.3 source archive](https://skse.silverlock.org/) · [Scaleform view scale modes](https://help.autodesk.com/cloudhelp/ENU/ScaleformStudio-Help/lua_ref/members.html)

This private stage supplies mode 1 only at that inventory call, only with an explicit trial setting and display dimensions wider than 16:9, allowing for integer rounding. The 32-bit Classic call site and the later CommonLib interface agree on the scale setter's slot. CommonLib identifies values 1 and 3 as ShowAll and NoBorder. [CommonLib Scaleform interface](https://parapets.commonlib.dev/GFxMovieView_8h_source.html)

The pinned hosted test compiles the staged loader and exercises the original call, with an authored MovieView receiver. It checks that the trial only changes the scale mode, with ordinary 16:9, unrelated movies, other scale modes, invalid dimensions and the disabled path keeping their original inputs. The SWF layout, pointer and touch alignment, and viewport presentation require comparison on iPhone. This trial is separate from the app and makes no change to the touch controls.

```sh
SKYRIM_TEST_OWNED_CLASSIC_LIFT=/path/to/generation \
  python3 -B -m unittest -v test_inventory_fit
```

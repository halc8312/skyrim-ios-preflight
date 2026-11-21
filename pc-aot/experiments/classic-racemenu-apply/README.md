# RaceMenu LE Nose Height preset application

RaceMenu LE 3.4.5 stores the stock facial slider values in
`morphs.default.morphs`. Its source maps Nose Height to
`kMorph_NoseDownUp`, stock morph index 1. The experiment applies that
one value through Skyrim's existing continuous-slider path. [RaceMenu serializer](https://github.com/expired6978/SKSEPlugins/blob/ea858fd0716a60c45fd572b558edd67da97fd132/skse/chargen/MorphHandler.cpp) ﾂｷ [RaceMenu stock slider mapping](https://github.com/expired6978/SKSEPlugins/blob/ea858fd0716a60c45fd572b558edd67da97fd132/skse/chargen/ScaleformFunctions.cpp)

The pinned LE 1.9.32 lift calls the slider setup routine `f_882290` from
the character menu update at return address `0x8859B4`, after its
FaceGen readiness check. The descriptor built from the localized
`sRSMNoseHeight` setting has continuous type 2 and stock morph index 1.
The stage verifies the two generated unit hashes and the relevant call
and field-read anchors. At the stock slider loop it accepts only that
descriptor and the current player's actor base. It substitutes the
validated value in the original frame's local slider argument, before
the existing UI and engine code use it. The stock setup and
`f_87F6E0` calls remain in place, including the other unmodified uses of
the character menu constructor.

The one-shot policy is called on the menu owner thread. The producer
binds a parsed preset to both the active profile and menu generations,
the menu, the preview actor and its base. The consumer supplies the live
scope and range when the reviewed descriptor is found. A different
generation, preview target or nonfinite and out-of-range value leaves
the engine argument untouched.

## Tests

From this directory, run `python3 -B -m unittest -v test_racemenu_apply`.
`SKYRIM_TEST_SANITIZERS=1` enables address and undefined-behavior
sanitizers. The optional `SKYRIM_TEST_OWNED_CLASSIC_LIFT` supplies the
pinned generation directory with `gen` and `source/runtime` inside.
The owned-source test syntax-checks the staged unit and confirms a
byte-for-byte restoration of its original contents. Authored fixtures
exercise one-shot admission and the injected argument substitution
through the original caller and descriptor conditions.

## App and iPhone verification

A private guest built with `--mod-hooks chargen_slider` exposes the
opt-in application callbacks. In its package detail screen, an enabled
RaceMenu preset can be queued for the profile selected in Settings.
The app validates and hashes the bounded file before saving a request
and repeats this check against the running profile on launch. Only the
guest's menu callback may claim the request. The stage verifies the
current player's actor base again before the stock slider call and
acknowledges the request after it returns. The app clears only that
same request on the main thread.

The iPhone test must show that a stock Nose Height value changes in
the menu, follows the expected face animation, persists after saving
and reloading a copied character, and remains unchanged in another
profile. Other RaceMenu sliders and extension data still need their
own review.

Enhanced Character Edit's numbered slot is already imported into
`CME_save`. In the owned ECE 1.59 archive, the human-race INI specifies ID
12, type 200 and the left/right NoseDown and NoseUp morphs. Its LE DLL
normalizes the INI type to continuous morph type 0 with option flags
3. The default descriptor is -1 to +1. The slot writer formats its
stored binary64 value directly as decimal, and the slot reader passes
its stored value directly to the Scaleform position result. ECE's optional
SliderScale changes the active bounds. The author describes retention
of saved values and clamping when loading in an environment with a
narrower range. [ECE author documentation](https://tktk1.net/skyrim/mymod/enhancedcharacteredit/)

The pinned stock descriptor also starts at -1 to +1. Its FaceGen
option 1 selects NoseDown for negative values and NoseUp for positive
values. The ECE configuration can select and merge multiple TRI
sources for the same named morph. The matched names, default bounds
and decimal transport leave the ECE UI-to-output sign and magnitude
and the resulting FaceGen effect under the selected race and setup
unverified. Use a pair of actual exports with one known directed
change on the same character, and compare the accepted stock input
and face effect before connecting ECE to the stock apply path.

The optional host audit in `tests/test_mod_ece_slot.py` checks the exact
owner DLL's descriptor, writer and reader without executing it. Set
`SKYRIM_TEST_ECE_CME_LIBRARY` to that unmodified LE DLL and use GNU
objdump. The provided author preset and its altered test copy still
exercise decimal parsing only.

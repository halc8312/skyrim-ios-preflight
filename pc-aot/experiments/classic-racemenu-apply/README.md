# RaceMenu LE Nose Height preset application

RaceMenu LE 3.4.5 stores the stock facial slider values in
`morphs.default.morphs`. Its source maps Nose Height to
`kMorph_NoseDownUp`, stock morph index 1. The experiment applies that
one value through Skyrim's existing continuous-slider path. [RaceMenu serializer](https://github.com/expired6978/SKSEPlugins/blob/ea858fd0716a60c45fd572b558edd67da97fd132/skse/chargen/MorphHandler.cpp) · [RaceMenu stock slider mapping](https://github.com/expired6978/SKSEPlugins/blob/ea858fd0716a60c45fd572b558edd67da97fd132/skse/chargen/ScaleformFunctions.cpp)

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

The next app step is to bind the selection from the active profile to
the menu's owner thread and a fresh menu generation. The callbacks need
a `CHARGEN_SLIDER` guest ABI table, build and install checks, and a
profile action that accepts a parsed RaceMenu preset. The iPhone test
must show that a stock Nose Height value changes in the menu, follows
the expected face animation, persists after saving and reloading a
copied character, and remains unchanged in another profile. Other
RaceMenu sliders and extension data still need their own review.

Enhanced Character Edit's numbered slot is already imported into
`CME_save`. A controlled pair of real ECE 1.59 slots from the same
character with one stock slider changed is needed to verify the
format used for applying its contents. [ECE author documentation](https://skyrimshot.blogspot.com/2017/06/enhanced-character-edit.html)

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
stored binary64 value directly as decimal. The reader multiplies a
legacy ID 12 row by 1.5 unless it has already seen the version 2.0
marker. The menu uses the parsed position and clamps it to the active
slider range. ECE's optional SliderScale changes that range.
[ECE author documentation](https://tktk1.net/skyrim/mymod/enhancedcharacteredit/)

The pinned stock descriptor also starts at -1 to +1. Its FaceGen
option 1 selects NoseDown for negative values and NoseUp for positive
values. The ECE configuration can select and merge multiple TRI
sources for the same named morph. The pinned ECE implementation selects
NoseDown for a negative coefficient and NoseUp for a positive one,
then uses the absolute value as the weight. The standalone slot conversion
uses the same signed coefficient for stock option 1 with the audited
default range. A same-character export pair remains useful for comparing
rendered faces on device and verifying the active race and head parts.

The optional host audit in `tests/test_mod_ece_slot.py` checks the exact
owner DLL without executing it. Set `SKYRIM_TEST_ECE_CME_LIBRARY` to
the unmodified LE DLL and use GNU objdump. For the other checks, set
`SKYRIM_TEST_ECE_RELEASE_MENU` to the shipped `racesex_menu.gfx`,
`SKYRIM_TEST_ECE_RELEASE_PRESETS` to the shipped Presets directory,
and `SKYRIM_TEST_OWNED_CLASSIC_LIFT` to the reviewed stock generation.

The profile check verifies the actual loose winners in a held, already-deployed
Data view. It checks the exact default `FemaleHeadCharGen.tri`, the two
race and slider INIs, and all five referenced female TRI inputs. An alternate
winner, an extra top-level CME INI (such as `SliderScale.ini`), an ambiguous
path or a symlink makes the check fail. Only the file digests and sizes are
committed. Set `SKYRIM_TEST_ECE_DEFAULT_DATA` to private Data contents from
the released `00 Main` package to run the hosted test against actual bytes.

The owned LE base game associates `NordRace` with `FemaleHeadNord` and the
`Actors/Character/Character Assets/FemaleHeadCharGen.tri` chargen path. The
standard ECE plugin retains that head part and the default package replaces
the same loose path. Its NoseDown and NoseUp records equal those in the
ECE base female TRI. They differ slightly from the stock LE records in
606 and 902 components, respectively, with a maximum difference below
2.35e-5 TRI units. Exact matches require the selected ECE asset.

The `# character race: …` slot line is display text. It has no authenticated
race FormID or sex. The slot parser returns the raw value, and the file check
does not inspect the running character or resolve the engine's resource
winner. Application still requires validation of that receiver and resource,
and a comparison using two actual exports.

A separate hosted test uses the real launch-view overlay and the staged
Skydream Kernel32 file shims. It reads the winning loose TRI by the
Data-rooted form of the FaceGen resource name and checks the full file
digest. Reversing package priority changes the returned bytes without
altering the original game directory. The fixture runs beneath the game's
resource layer. The in-game choice between the loose file and BSA remains
to be observed.

### FaceGen source diagnostics

A private Classic guest can include `ece_tri_observe` in its
`--mod-hooks` list. On a pinned detailed-diagnostics launch, the app
requires the published active profile to list the ECE plugin and the
deployed Data to pass the default ECE 1.59 winner check. The check holds
the same read-only loose file descriptor that passed the digest check.

The pinned `BSResourceFaceGenBinaryFile` vtable maps the loader to
`f_59F270`. The observer takes the exact path passed to the original
`f_A3F810` call and uses the stream from that call's output slot. It
scopes the stock `f_59EF90` parser and observes its original return.
For the loose resource stream, a positive read must come from the
reviewed `f_A58110` import and use the same stream's Win32 handle.
The prepared Skydream shim checks the descriptor against the audited
loose winner's inode and metadata after the original read. Archive
member streams constructed by `f_A3B2D0` and `f_A3B700` are classified
from their own type, independent of whether an archive was opened in
this frame. Other stream types or missing backing read evidence are
classified as unknown.

The numeric record is `IOS_ECE_TRI schema=1 source=N parsed=N`. Source 1
is the audited loose winner, 2 is a reviewed archive stream, and 0 is
unknown. Parsed 1 is success from the original parser. Each combination
is logged once per launch. A failed parse uses source 0. No file names,
addresses or character details are exported. An already cached resource
may bypass this parser, so use a fresh launch for a device check.
The test exercises the exact staged code and the prepared file wrappers
against authored guest objects. A first real-device observation and an
ECE-to-stock morph comparison still need to be made.

### Female Nord nose diagnostics

The optional hosted witness stages only the same reviewed original-body lift
used in the other Classic experiments. It sees the menu through the
engine's retained `RaceSexMenu` frame dispatch before the stock slider
callback. It checks that the live preview is the player's NPC base, the
base has female sex and NordRace, the actor's effective race agrees, and
the female race entry selected by the menu agrees. It observes the
existing stock Nose Height call without changing its value.

The witness then follows the face node returned by the original player
method and the retained geometry selected by the stock head-part lookup.
It accepts the expected female Nord head part only. It observes the
actual TRI pathname chosen by the head part, and counts a named NoseUp
or NoseDown record when the stock morph lookup returns one for that
geometry. The report contains booleans only. An instance or resource
address stays inside the synchronous menu scope.

The temporary object used by the TRI parser is separate from the actual
TRI that the morph code uses. For the reviewed direct-parser route,
`f_5A0710` turns a successful parse into a resource entry with
`f_5A0550`. The witness requires an independently audited source receipt
for that invocation, the new entry's original runtime TRI, the same
entry returned by the stock resource lookup, and that same runtime TRI
given to the morph code. Loss of the entry, a cached lookup, or lack of
a source receipt leaves fresh source unproved. The alternate parser
route remains unverified.

With the owned generation set in `SKYRIM_TEST_OWNED_CLASSIC_LIFT`, run
`python3 -B -m unittest -v test_ece_nose_witness` from this directory.
The test syntax-checks all staged units and runs the fixed lookup and
parser wrappers and named morph selector with authored backing records.
The hosted fixture supplies the source receipt. The app uses the same
witness with `ece_nose_observe` and `ece_tri_observe` both compiled, the
existing detailed-diagnostics and ECE profile checks passed, and preset
substitution disabled. It verifies that the original file read in the
same parse used the already-audited loose winner. Idle menu frames leave
the attempt budget untouched. At most 64 stock Nose Height scope attempts
are observed per launch, with one line per observed combination.

The record is `IOS_ECE_NOSE schema=1 female_nord=N head=N geometry=N path=N
fresh=N record=N`, with six Boolean fields. A cached lookup or separate
successful resource log can leave provenance unknown. First real-device
observations are still needed. The witness leaves preset and game state
untouched.

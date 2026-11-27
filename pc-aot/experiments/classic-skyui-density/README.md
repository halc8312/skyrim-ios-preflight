# SkyUI 5.1 phone list layout

The hosted planner defines Compact and Large Text presets for the Inventory and Magic menus. The inventory presets emphasize name, weight and value. The magic presets emphasize name, cost and school. Both preserve equipment and item icons. Dedicated Shouts and Active Effects column lists keep their stock structure.

The setting is opt-in per profile. The host checks the original SkyUI 5.1 archive, SKSE scripts and loose menu/config overrides before queuing extra arguments. On the keys command, it appends keys to the temporary GFx argument vector. On the values command, it reads the live movie, checks its initial `LOAD_FILE` phase, exact key-array length and matching suffix, then appends the values. The stock value command only iterates over the original 128 values when any check fails, even if the keys had already been appended. The original Papyrus arrays and saved MCM overrides are never written. [SkyUI configuration delivery](https://raw.githubusercontent.com/schlangster/skyui/8354287/dist/Data/Scripts/Source/SKI_SettingsManager.psc) · [ConfigManager](https://raw.githubusercontent.com/schlangster/skyui/8354287/src/Common/skyui/util/ConfigManager.as) · [ListLayout](https://raw.githubusercontent.com/schlangster/skyui/8354287/src/Common/skyui/components/list/ListLayout.as)

The fixture compiles the production adapter against a simulated live movie. It checks the outgoing arguments, fallback when key/value calls land in different movies, rejection before and after initial load, and cleanup after failed Scaleform reads. The original SkyUI SWFs' actual readiness timing, private field access and visible fonts, columns and sort selections still need an iPhone check. A loose file check cannot establish which SWF a competing BSA supplies, so the live config shape is checked again immediately before applying values.

```sh
SKYRIM_SKYDREAM_SOURCE=/path/to/pinned/skydream \
  python3 -B -m unittest discover -s tests -p test_classic_skyui_regression.py -v
```

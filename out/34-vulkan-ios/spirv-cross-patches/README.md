# SPIRV-Cross patches (spec 34 clarification 1)

All 47 of steamac's SPIRV-Cross patches (github.com/StonkGuy/steamac, commit `56aa855`,
`host/moltenvk/patches/spirv-cross/`, Apache-2.0; its series has no 0038–0041), with steamac's file
names and author lines. `build-moltenvk.sh` applies them, in name order and with no fuzz, to its
copy of UTM's SPIRV-Cross fork (`939b40b3`), which the MoltenVK build then uses through
`fetchDependencies --spirv-cross-root`. Why each is adopted, and why 0001 is kept: `../CHANGES.md`
("Clarification 1"). Game names in their messages are replaced by "a game"; nothing else is changed.

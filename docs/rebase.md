# Rebasing `martin` onto upstream

Quick checklist for pulling in `pchambre/pc1500emu` upstream changes.

## 1. Check whether a rebase is needed

```sh
git fetch upstream
git log --oneline master..upstream/master
```

If that's empty, nothing to do. Otherwise upstream has moved and `martin`
(and/or local `master`) should be rebased onto `upstream/master`.

## 2. Rebase

```sh
git rebase upstream/master
```

### Expected conflicts: version-number collisions

Both this branch and upstream bump `CMakeLists.txt`'s `project(...VERSION
x.y.z...)` and add a top `CHANGELOG.md` entry on every commit (project
convention). Since both sides do this independently, every rebased commit
that touches either file conflicts, every time -- this is expected, not a
sign of a bad rebase.

Resolution pattern per conflicting commit:
- `CMakeLists.txt`: take the *next* free version number after whatever
  upstream now has at that point in the rebase (i.e. keep incrementing,
  don't reuse a number upstream already claimed).
- `CHANGELOG.md`: keep both sides' entries, reordered so the renumbered
  entry sits in the right chronological slot (entries are newest-first).
  Drop the duplicate leftover half of the diff3 markers once merged.

### Other likely conflicts

- `src/hoststate/state_file.h` (`kStateFileVersion`): if both sides bump
  the state-file format version for unrelated reasons, merge into one new
  version number with a combined comment explaining both changes -- don't
  pick one side and drop the other's bump.
- `src/bus/bus.h` RAM-config setters: if both sides touch the same setter
  (e.g. `setExtRam0000Size`, `setCe163Enabled`, `setCe168nEnabled`), keep
  both edits -- they're usually independent fixes to the same function,
  not alternatives. Worth a quick sanity pass afterward: check whether a
  fix added to one setter (e.g. a `reseedReserveArea()` call) should also
  apply to sibling setters that didn't get touched in either commit.

Continue as usual: `git add <resolved files>`, `git rebase --continue`,
repeat per commit.

## 3. Verify

```sh
cd build
cmake --build . -j"$(sysctl -n hw.ncpu)"
ctest --output-on-failure
```

**`app_config_test` fails on macOS every time -- this is a known
pre-existing failure, not something the rebase caused.** It asserts
Windows-style paths (`C:/roms/rom1.bin`, `C:/saves/session1.state`) that
don't round-trip through macOS path handling. Confirmed by building
`upstream/master` directly in a scratch worktree: same 3 failing checks,
same file (`tests/app_config_test.cpp:64,66,208`). Ignore this one
failure; all other tests should pass. If a *different* test fails, or
`app_config_test` fails on more/different assertions than those three,
that's a real regression worth investigating.

## 4. Push

The rebase rewrites history, so `martin` will have diverged from
`origin/martin`. Push with `--force-with-lease`, and confirm with the user
first since it's a shared/remote-visible operation.

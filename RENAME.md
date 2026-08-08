# Repository housekeeping

The rename to **epoch-equinox** and leaving the fork network are both done.
This file is what's left over.

## Repointing an existing clone

The old URL only redirects, so local clones should be moved across:

```sh
git remote set-url origin https://github.com/ZakyPew/epoch-equinox.git
```

## Description and topics

The project name no longer says what it is, so the repo description and topics
now carry discoverability on their own. Suggested description:

> Oracle of Ages and Oracle of Seasons, recompiled to native C from your own
> ROMs. Modern launcher, IPS/BPS mod support, optional 3D diorama mode.

Topics worth adding: `game-boy-color`, `static-recompilation`,
`decompilation`, `zelda`, `sdl2`, `modding`, `port`.

## Optional, and probably not worth it: purge the old blobs

`.git` is ~22 MB while the working tree is ~2 MB, because the generated C that
used to live here still exists in past commits. A fresh clone therefore still
pulls ROM-derived code.

Worth knowing before acting on that: the same code is still public in
`GB-Recomp/tlozooa`, where it came from. Purging this copy changes what *this*
repo hands out; it doesn't remove anything from the world.

If you want it gone anyway:

```sh
pip install git-filter-repo
git filter-repo --path-glob 'tlozooa_*' --path assets_manifest_tlozooa.h --invert-paths
git push --force --all
git push --force --tags
```

Every commit hash changes. Fine for a project with few contributors;
coordinate first if that stops being true. GitHub also keeps unreferenced
objects reachable by SHA for a while afterwards, so Support would need to run
a `gc` for it to be thorough.

22 MB is not a burden, and the history is the only record of how the project
got here. My advice is to skip it.

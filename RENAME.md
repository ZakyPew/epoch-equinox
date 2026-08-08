# Renaming and detaching

Two steps that can only be done from the GitHub web UI, not from a clone.
Both are safe, and neither loses anything.

## 1. Rename the repository

**Settings → General → Repository name** → `oracles-recompiled` → **Rename**.

GitHub permanently redirects the old URLs, so:

- existing clones keep working (`git push` still lands, just warns)
- old links, the merged pull requests and their history all survive
- forks and stars come along

To point an existing clone at the new name explicitly:

```sh
git remote set-url origin https://github.com/ZakyPew/oracles-recompiled.git
```

While you're on that screen, the **description** is still the old one. Something like:

> Oracle of Ages and Oracle of Seasons, recompiled to native C from your own
> ROMs. Modern launcher, IPS/BPS mod support, and an optional 3D diorama mode.

Topics worth adding: `zelda`, `game-boy-color`, `static-recompilation`, `decompilation`, `oracle-of-ages`, `oracle-of-seasons`, `sdl2`, `modding`.

## 2. Detach the fork

The repo is still marked *"forked from GB-Recomp/tlozooa"*, which is no longer
true of its contents — none of that code is here any more.

**Settings → General → Danger Zone → Leave fork network.** If that option
isn't shown for this repo, [GitHub Support](https://support.github.com/request)
will do it; the request is routine.

Detaching gives the project:

- its own issues, pull requests, stars and watchers
- its own presence in search (forks are hidden from GitHub code search by default)
- no "forked from" banner implying it's a variant of something else

Nothing is lost — commits, branches, releases and PR history all stay.

## 3. Optional: drop the old blobs from history

`.git` is ~22 MB even though the working tree is ~2 MB, because the deleted
generated C still exists in past commits. Two ways out:

**Rewrite history** (keeps the repo, its stars and its issues):

```sh
pip install git-filter-repo
git filter-repo --path-glob 'tlozooa_*' --path assets_manifest_tlozooa.h --invert-paths
git push --force --all
git push --force --tags
```

Force-pushing rewrites every commit hash. Fine for a project with few
contributors; coordinate first if that changes.

**Or start clean** — simpler, and gives a repo that was never a fork:

```sh
# from a checkout of main, with no build/ or roms/ present
rm -rf .git
git init -b main
git add -A
git commit -m "Initial commit: oracles-recompiled"
git remote add origin https://github.com/ZakyPew/oracles-recompiled.git
git push -u origin main
```

That loses the commit history and the merged PRs. Worth it only if you'd
rather the project have a clean origin story than a record of how it got here.
Renaming and detaching are enough on their own — this step is genuinely
optional.

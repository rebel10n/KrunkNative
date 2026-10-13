# Rules for parallel agents (coordinator)

The shared checkout /home/user/src/KrunkNative is the integration tree: it must build
(native + web) at all times, and the coordinator commits from it.

1. Do your work in a private git worktree, not in the shared checkout:
     git -C /home/user/src/KrunkNative fetch -q
     git -C /home/user/src/KrunkNative worktree add --detach ~/.cache/kn-wt/<agent> origin/main
     # game content is gitignored: link it in
     for d in $(ls -d /home/user/src/KrunkNative/assets/*/); do n=$(basename $d); [ -e ~/.cache/kn-wt/<agent>/assets/$n ] || ln -s $d ~/.cache/kn-wt/<agent>/assets/$n; done
   Build in its own dir (~/.cache/kn-wt/<agent>-build). /tmp is quota-limited (25 GB per user), so use ~/.cache.
2. Rebase onto origin/main often (`git fetch && git rebase origin/main` in your worktree),
   because the coordinator commits other agents' work all the time.
3. Deliver work as a patch against origin/main HEAD, made with `git diff --binary origin/main`
   in your worktree. The patch must apply, build native + web and pass your tests on a clean
   worktree before you send its path. The coordinator applies and commits it. Never commit.
4. If you already have uncommitted edits in the shared checkout, move them into your
   worktree (make a patch, apply it there, then revert only YOUR hunks in the shared
   checkout). Leave the shared checkout building. Never revert another agent's hunks.
5. Dependencies on another agent's uncommitted work: ask the coordinator to commit theirs first,
   or temporarily apply their patch in your worktree only.
6. Remove your worktree when you've delivered (`git worktree remove --force <dir>`).
7. Never use `git stash` (or `git checkout -- .` / `git reset --hard` on shared refs): refs/stash is shared by
   every worktree of this repo, so stashes from different agents mix. Save work in progress as a file
   instead: `git diff --binary > ~/.cache/kn-wt/<agent>-wip.patch`. Never pop or drop an entry you didn't create.
8. Never commit, push, or move branches/tags; the coordinator does that.
9. "kn-coord-check" in briefs means: a full native Debug build plus the Emscripten web build of your tree
   (`cmake -S <wt> -B <wt>-build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build <wt>-build`, then
   `source ~/emsdk/emsdk_env.sh && emcmake cmake -S <wt> -B <wt>-web -DWEB_DIST_DIR=<wt>-webdist && cmake --build <wt>-web`).
10. Many agents run the e2e scripts at once. New or changed e2e tests take their ports from
    `kn_proto.free_ports()` (server/tests/kn_proto.py), never fixed numbers, and must not kill processes
    they didn't start.
11. Before generating your patch: `git fetch origin && git rebase origin/main` in your worktree (or regenerate the
    patch from a fresh worktree of origin/main with your changes applied), then `git diff --binary origin/main`.
    A patch made from a stale base silently reverts other people's commits (it happened: 557c53e put back the
    push trigger of Deploy kru). State the exact base commit in your report.
12. Rebase with git (commit your work in the worktree's detached HEAD and `git rebase origin/main`, or apply your
    own patch onto a fresh worktree of origin/main); never bring a worktree "up to date" by copying files, or by
    `git diff` against a newer origin/main than the files you started from. Either way, the diff then removes every
    change that landed in those files in between, and the patch still applies cleanly (it happened: 0bf1b86
    undid all of 72c5dec). Check before sending: `git diff origin/main --stat` must list only files you changed on
    purpose, and no hunk may remove lines you didn't write.

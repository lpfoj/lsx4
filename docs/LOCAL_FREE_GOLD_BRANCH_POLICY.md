# Local FREE/GOLD branch policy

> Any performance/FPS validation on these branches must first pass the mandatory clean-host gate
> in [`PERFORMANCE_TEST_PREFLIGHT.md`](PERFORMANCE_TEST_PREFLIGHT.md) by running
> `scripts/preflight-vivo-performance-test.ps1`. Close the game between phases; measurements are
> invalid while diagnostic processes occupy CPU cores.

This repository has two release lines with deliberately asymmetric history.

```text
public remote (older by about one month)
  lsx4/main                    origin/main (Funnel)
       |                              |
       v                              v
local FREE
  main                          funnel-arm: main
       |                              |
       +---------- one way ----------+
                    FREE -> GOLD
                         |
                         v
local GOLD
  gold/main                     funnel-arm: gold/main
  gold/* feature branches       funnel-arm: gold/* feature branches
```

## Invariants

1. Local `main` is the current FREE application state. It may be ahead of the public remote.
2. `gold/main` starts from local FREE `main`, may add Gold-only commits, and must retain them when
   it receives later FREE changes.
3. Integration is one-way: merge local `main` into `gold/main`. Never merge `gold/main` or another
   `gold/*` branch into FREE `main`.
4. Gold updates consume only local FREE branches. The update helper does not fetch or push.
5. No branch is pushed automatically. FREE is pushed only on an explicit owner request.
6. A Gold push requires a warning and a second explicit confirmation. The local pre-push hook
   rejects it by default. After the owner confirms that they remember the risk and still insist,
   the one-command override is `LSX4_GOLD_PUSH_CONFIRM=I_REMEMBER_AND_INSIST`.
7. Historical checkpoints, optimization branches, tags, and worktrees are retained.

## Switching editions

Switch both the parent LSX4 repository and nested Funnel repository together:

```powershell
& '.\scripts\switch-lsx4-edition.ps1' -Edition Free
& '.\scripts\switch-lsx4-edition.ps1' -Edition Gold
```

The helper refuses to switch while tracked source changes are present. Generated dirt inside
third-party submodules does not select an edition and is ignored by this check.

## Updating Gold from current local FREE

First commit all FREE work on both local `main` branches. Then switch to Gold and merge:

```powershell
& '.\scripts\switch-lsx4-edition.ps1' -Edition Gold
& '.\scripts\update-gold-from-free.ps1'
```

The updater merges Funnel `main` into Funnel `gold/main`, records the resulting Gold Funnel commit
in the parent Gold history, and then merges LSX4 `main` into LSX4 `gold/main`. It verifies that each
Gold branch contains its corresponding local FREE `main`. Gold-only commits remain in history.

To return to FREE development:

```powershell
& '.\scripts\switch-lsx4-edition.ps1' -Edition Free
```

Do not use the Gold updater in reverse and do not configure FREE branches to track Gold branches.

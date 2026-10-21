# Contributing to LSX4

Bug reports, gameplay evidence, documentation and focused code changes are welcome. AI-assisted contributions are welcome too. The person submitting a change is responsible for understanding it and checking its results.

## Send a change

1. Fork [lpfoj/lsx4](https://github.com/lpfoj/lsx4) and create a branch in your fork from the current `main`.
2. Read [AGENTS.md](AGENTS.md), [ARCHITECTURE.md](ARCHITECTURE.md) and the instructions for the area you will change. The existing regression contract is mandatory for shared runtime work.
3. For a large architectural change, open an issue with the measured bottleneck, proposed ownership model and fallback before implementation. Small fixes can go straight to a PR.
4. Keep the patch focused. Separate behavior changes from formatting, renaming and unrelated cleanup.
5. Run the relevant checks from [Game regression safety](docs/GAME_REGRESSION_SAFETY.md). Record what actually ran and what could not be tested.
6. Push your branch to your fork. Open a PR with base `lpfoj/lsx4:main` and compare your fork's branch. GitHub calls this a pull request; it is the equivalent of an MR on other hosts.
7. Address review comments in the same branch. Merging requires maintainer review, not just a successful build or model approval.

You do not need to be a collaborator to send a PR from a public fork. We do not grant public write access to `main`. Do not include local-only development branches or private release material in a public PR.

## Working with AI models

- Give the model a bounded task, relevant source files, the failing observation and the invariants that must remain true. Ask for a proposed cause before a broad rewrite.
- Inspect the actual execution path. A setting selected in the UI does not prove that the native backend uses it. Record requested and effective values.
- Treat model output as an unreviewed patch. Verify APIs, file paths, ownership, lock ordering and error paths against the code. A second model's agreement is not test evidence.
- Do not invent benchmark numbers, screenshots, test runs, compatibility marks or claims about how another emulator works. Separate observations, hypotheses and conclusions.
- Test behavior, not just the text of an implementation. Source-contract tests help, but do not replace executable tests or physical-device gameplay.
- Never weaken a test, capability guard, memory lease, save contract or synchronization rule just to make a patch pass. Explain and review any intentional contract change separately.
- Do not paste keys, tokens, private diagnostics, proprietary game files, firmware, personal saves or device identifiers into external model services. Use small synthetic examples and approved public source material.
- Keep generated code readable. Reuse the existing interface before adding another manager, toggle or fallback. Comments should explain a non-obvious invariant or tradeoff, not restate each line. Avoid unrelated comment removal and mass renaming.
- Disclose material AI assistance in the PR, including which parts were generated and how they were reviewed. Full chat transcripts are not required. A model's claim that it ran a test is not enough; include the real command and result.

## Report a game result

Include the game title and title ID, PS4 or PS5, app/build version, device model, Android version, GPU driver, settings and enabled patches. Describe the tested scene and actions, not just whether it boots.

For a regression, give the last known good build and the first bad build if available. For performance, compare the same save and scene with matched resolution, backend, settings, thermal conditions and diagnostic overhead. Separate loading/menu FPS from sustained gameplay FPS.

Do not upload games, firmware, personal saves or unredacted logs. Attach only evidence you have the right to share. Hide names, file paths, serial numbers and unrelated notifications. A game absent from the compatibility list is untested, not automatically broken.

The app's long-press compatibility mark is local. It does not automatically open a GitHub issue or publish a website report. User reports do not automatically promote a title into the curated Playable list.

## Review and acceptance

Use the PR template. List affected platforms, the failure mechanism, the fallback and the evidence for the final artifact. Mark checks as passed, failed or not run, with a reason. Do not combine results from different APKs into one acceptance claim.

Maintainers may request broader gameplay coverage when a change touches shared memory, JIT/HLE, synchronization, shader translation or default profiles. A documentation-only PR does not need a phone test. New compatibility does not compensate for breaking an established game, and higher FPS does not compensate for corrupt graphics, missing audio or damaged saves.

No contribution may contain game assets, console firmware, upload keystores, access credentials or private build artifacts. Preserve third-party attribution and comply with the repository's licenses. Do not remove attribution because code was moved or rewritten with a model.

For GitHub's fork workflow, see [Creating a PR from a fork](https://docs.github.com/en/pull-requests/how-tos/create-pull-requests/creating-a-pull-request-from-a-fork).

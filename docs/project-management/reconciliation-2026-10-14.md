# Project reconciliation — 2026-10-14

Receipt for the Project sync after the v0.9.52 release (published 2026-10-14T04:07:16Z). It records the v0.9.52 release record, two closed Issues with their fix evidence, two open Android Issues, the Android experimental label removal, the render-pass merge step that is already on `main`, the PM4 assessment, the mod-loader progress, and two maintainer TODOs that were added as backlog drafts. No Issue, pull request, comment, commit or push was changed by this sync.

## Run

From the worktree on `docs/pm-sync-after-v0952` (base `origin/main` 128e36d5):

- Plan 1: `{"created": 6, "updated": 4, "unchanged": 304, "conflicts": 0, "operations": 57}`.
- `python -B tools/project_management/sync.py --apply`: same counts, exit code 0. The report went to the scratchpad, not `out/`.
- Plan 2 (after the apply): `{"created": 0, "updated": 0, "unchanged": 314, "conflicts": 0, "operations": 0}`, exit code 0.
- Readback (`gh project item-list 3 --owner freefrank --format json`): 315 items (314 managed records plus the keyless Issue #17 link). Every touched item was read back: Status, Delivery, Release, Kind, Area, start date and draft body presence matched. The full Evidence text was not compared word by word; the Evidence lengths are all under 1,000 characters.
- Round 2 (coordinator decisions, same day): plan `{"created": 1, "updated": 1, "unchanged": 313, "conflicts": 0, "operations": 13}`; apply same counts, exit 0; re-plan `{"created": 0, "updated": 0, "unchanged": 315, "conflicts": 0, "operations": 0}`, exit 0. Readback: 316 items; `render-pass-merging-v100` and `issue-492-android-black-artifacts-app-switch` matched the expected values and bodies.
- Project: <https://github.com/users/freefrank/projects/3>

Live GitHub checks before writing: `gh pr list --state all` (PR states and merge dates for #440 to #489) and `gh pr view` for #414, #403, #404, #405, #459, #478 and #457 (body); `gh issue list --state all` and `gh issue view` for #441, #453, #454, #456, #477, #490, #491 and #492, with events and comments for #454 and #477; `gh release view v0.9.52` (published, not a draft) and `gh release view v0.9.52 --json assets`; `git merge-base --is-ancestor` for #414 (in v0.9.32 and v0.9.52, not v0.9.22), and for #455, #478, #481, #482 and #486 against `v0.9.52`.

## Changed records

| Record | Before | After |
| --- | --- | --- |
| `experimental-android-platform` | Title "Experimental Android platform support"; In Progress; Released; Release v0.8.5 | Title "Android platform support (no longer experimental)"; Done; Released; Release v0.8.5 (unchanged). Evidence starts with the 2026-10-14 close (PRs #481 and #482, in v0.9.52) and keeps the unverified list. Body has a "Status — 2026-10-14" section. |
| `remove-pm4-translator` | Paused / Deferred | Paused / Deferred (unchanged). Evidence and body add the 2026-10-13 assessment: not worth it before v1.0 (upper bound about 1–1.4 ms per thread vs a ~4 ms gap; PR #89 measured no gain). |
| `render-pass-merging-v100` | Paused / Deferred / After v1.0.0 | In Progress / In progress / v1.0.0 (round 2, coordinator decision: the maintainer asked to continue on 2026-10-14). Evidence records the branch `perf/constant-edram-transfer` (GitHub, Gitea and local; no PR yet), the shipped step PR #414 (merged 2026-10-11, in v0.9.32; phase 1 about 0.2 ms; TB321FU passes 291 to 172), the 2026-10-14 hot 720p baseline (p95 21.4-21.6 ms vs the 18 ms gate) and the open measurement checkpoint. Body has a "Status — 2026-10-14" section. No result is accepted. |
| `texture-replacement-mod-loader` | In Progress / Released / v0.7.0 | Unchanged fields. Evidence now lists the v0.9.52 work (#463, #467 to #469, #472, #473, #474, #475, #476, #480, #486), keeps the remaining scope, and adds the 4 GiB limit as a pending maintainer decision. Body has a 2026-10-14 reconciliation note. |
| `android-frame-time-v100` | In Progress / v1.0.0 | Unchanged. #459 and the 2026-10-12 p95 gate were already recorded in the 2026-10-12 receipt. |

## Created records

| Record | Type | Status / Delivery / Release | Basis |
| --- | --- | --- | --- |
| `release-v0-9-52` | Draft (release record) | Done / Released / v0.9.52, dates 2026-10-14 | Published release; five assets with GitHub API size and digest; published, not accepted. Other PRs stay in CHANGELOG.md and the release page, as with `release-v0-9-22`. |
| `issue-454-steam-face-buttons` | Issue #454 (linked) | Done / Released / v0.9.52, start 2026-10-12 | Fix PR #455 is in v0.9.52. The maintainer closed the Issue as completed at 2026-10-14 05:22Z. No reporter confirmation is recorded. |
| `issue-477-android-startup-display-setup` | Issue #477 (linked) | Done / Released / v0.9.52, start 2026-10-14 | Fix PR #478 is in v0.9.52. The reporter wrote "it runs properly now" at 04:43Z and closed the Issue. This confirms startup on the reporter's device only. |
| `issue-491-android-no-sound-after-app-switch` | Issue #491 (linked, open) | In Progress / In progress / no Release, start 2026-10-14 | Open. Analysis in progress. No fix is recorded; PR #461 is not claimed as the fix. |
| `dlss-nr-settings-preview-blank` | Draft (maintainer TODO) | Todo / Not started, Area Settings | Maintainer note of 2026-10-13. Not an Issue; no owner, priority or date. |
| `fg-multiplier-restart-required` | Draft (maintainer TODO) | Todo / Not started, Area Frame generation | Maintainer note of 2026-10-13. Not an Issue; no owner, priority or date. |
| `issue-492-android-black-artifacts-app-switch` | Issue #492 (linked, open; round 2) | In Progress / In progress / no Release, start 2026-10-14 | Open, same reporter as #491; analysis in progress together with #491. No fix is recorded. |

## Conflicts with the brief and judgment calls

1. **Render-pass merging (resolved in round 2).** The first pass kept the item Paused / Deferred / After v1.0.0 because no open PR existed for the constant-EDRAM work. The coordinator then confirmed that the work is active: the maintainer asked to continue on 2026-10-14, branch `perf/constant-edram-transfer` exists on GitHub, Gitea and locally, and a measurement checkpoint is running on the TB321FU. The item is now In Progress with the v1.0.0 target. The 2026-10-12 pause is recorded as superseded in its body. No pass-merge result is accepted.
2. **#477 wording.** The brief said "awaiting reporter". The live state is different: the reporter confirmed startup on v0.9.52 and closed the Issue himself at 04:43Z. The record is Done with that scope. The title's "crash after shader download" is not covered by any recorded test.
3. **Closed Issues without reporter confirmation (#454).** The maintainer closed it. I used Done with the missing confirmation written in Evidence, which matches the existing closed-Issue records (for example #199, #251, #253 and #214). "Awaiting validation" was not used for these two; a parent decision could change that.
4. **Android experimental label.** The item is Done as instructed. The unverified scope (16 KB pages, other devices and GPUs, audio, controller hot-plug, acceptance) stays in Evidence and body, so the closure does not claim it.
5. **DLSS-NR preview and FG multiplier.** I added them as backlog drafts, following the `streamline-vulkan-present-after-write` precedent for maintainer-raised backlog. They have no priority, owner or date.

## Items not mapped

- **#453** (open, created 2026-10-12): the game keeps asking to download shaders. PR #457 does not cite it, so it is not linked to the shader-bundle work.
- **#441 and #456** (closed 2026-10-13): faster menus and saving, and faster item interaction. The closest record is `loading-and-save-speed-2026-10-10`, but its scope (loading and fewer steps) differs from an 8x-speed request. Not edited.
- **#490** (closed 2026-10-14, "Mods on Linux?"): not mapped.
- **PR #485** (Mali PanVK recommendation): not linked to `issue-214-mali-black-screen`. That record says no Mali device was tested, and #485 is not claimed as its fix.
- **PR #463 to #476 and #486** beyond the mod-loader Evidence: they are described in the release record only.

## Left for the maintainer or parent

- `docs/ROADMAP.md` and `docs/ROADMAP.zh-CN.md` still describe Android as experimental and still carry the PM4 entry. These files are outside the authorized scope, so they were not edited. They need a change in the roadmap pair, by the parent or docs_sync.
- Whether to close `texture-replacement-mod-loader`. It stays In Progress.
- The 4 GiB archive limit for full-map model mods on discs 3 and 4 (decision pending). Note only; no record until decided.
- The README table has no row for the 2026-10-12 receipt. It was not backfilled here.
- The sync-state.json file was rewritten by the apply and is uncommitted, as are items.json, this receipt and the README row. The main agent reviews and ships them.

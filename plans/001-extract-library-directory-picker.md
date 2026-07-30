# Plan 001: Extract the library directory picker model

> **Executor instructions**: Follow this plan step by step. Run every
> verification command and confirm the expected result before moving to the
> next step. If anything in the "STOP conditions" section occurs, stop and
> report; do not improvise. When done, update the status row for this plan in
> `plans/README.md` unless a reviewer told you they maintain the index.
>
> **Drift check (run first)**:
> `git diff --stat b6aaf64..HEAD -- Makefile src/library_tui.c src/library_directory_picker.c src/library_directory_picker.h tests/test_library.c tests/test_library_directory_picker.c tests/test_main.c tests/test_support.h`
> If an in-scope file changed since this plan was written, compare the
> "Current state" excerpts against the live code before proceeding. Treat a
> mismatch in the folder-picker code as a STOP condition.

## Status

- **Priority**: P1
- **Effort**: M
- **Risk**: MED
- **Depends on**: none
- **Category**: tech-debt
- **Planned at**: commit `b6aaf64`, 2026-07-30

## Why this matters

`src/library_tui.c` is 2,859 lines, but file size alone is not the defect:
`src/reader_tui.c` is larger. The concrete layering problem is that
`library_tui.c` owns the directory picker's filesystem enumeration, fff index
lifecycle, query results, selection model, path navigation, ncurses rendering,
and application command emission. Extracting the non-rendering picker model
creates one ownership boundary that can be unit-tested without a PTY and keeps
the ncurses controller focused on presentation and input dispatch.

Do not turn the C code into pseudo-C++. The target is a cohesive C module with
explicit state and lifecycle functions, matching the existing
`library_shelf.c`/`library_shelf.h` pattern. Do not add inheritance, vtables,
generic callbacks, dependency-injection frameworks, or configurability.

## Current state

- `src/library_tui.c:60-71` defines all owned picker state inside the ncurses
  controller:

  ```c
  typedef struct MereaderTuiLibraryDirectoryPicker {
    char *path;
    char **names;
    size_t length;
    size_t capacity;
    MereaderTuiTextInput query;
    MereaderTuiSearchIndex search;
    MereaderTuiSearchDirectories matches;
    size_t selected;
    size_t top;
    bool scanning;
  } MereaderTuiLibraryDirectoryPicker;
  ```

- `src/library_tui.c:1179-1407` mixes model work with controller state. It
  frees owned paths and indexes, calls `opendir`/`readdir`/`fstatat`, resolves
  paths, starts or reuses fff indexes, filters results, polls scan completion,
  and computes entry counts.
- `src/library_tui.c:1414-1438` keeps selection and scrolling invariants in the
  controller even though they depend only on picker state and visible row
  count.
- `src/library_tui.c:2235-2334` performs parent and selected-entry path
  navigation directly against picker internals.
- `src/library_tui.c:1440-1697` is ncurses presentation and must remain in
  `library_tui.c`.
- `src/library_tui.c:2336-2394` is key dispatch and must remain in
  `library_tui.c`; it should delegate state transitions to the extracted
  module.
- `include/mereader-tui/library_shelf.h` plus `src/library_shelf.c` is the
  applicable project pattern: a named state struct, explicit build/free
  functions, `MereaderTuiError` propagation, replacement state constructed
  before old state is released, and no class emulation.
- `tests/test_library.c:1123-1238` contains PTY coverage for parent-folder and
  fuzzy folder selection. Those tests are characterization gates and must stay
  unchanged unless an include-only or registration-only change is required.

## Commands you will need

| Purpose | Command | Expected on success |
|---|---|---|
| Build and tests | `make test` | 153 existing tests plus new picker tests pass; CLI summary passes |
| Sanitizers | `make sanitize` | all native and CLI tests pass with no ASan/UBSan report |
| Rebuild normal binary | `make clean && make -j2 all` | exit 0 |
| Install verification | `make installcheck` | both staged system and user install tests pass |
| Diff hygiene | `git diff --check` | no output, exit 0 |

## Suggested executor toolkit

- Use the `agent-browser` skill after integration to recreate and visually
  inspect the folder-picker search state documented in the PTY test: open with
  `p`, type `clasic`, and confirm the query, ranked result, footer, and
  `#1e1e2e` background are unchanged.

## Scope

**In scope** (the only source and test files to modify):

- `src/library_directory_picker.h` (create)
- `src/library_directory_picker.c` (create)
- `src/library_tui.c`
- `tests/test_library_directory_picker.c` (create)
- `tests/test_main.c`
- `tests/test_support.h`
- `Makefile`
- `plans/README.md` (status only)

**Out of scope**:

- `src/reader_tui.c`; its size is not evidence that this extraction requires
  changing it.
- `include/mereader-tui/search.h` and `src/search.c`; the fff adapter already
  has the operations required by the picker.
- `tests/test_library.c`; preserve the PTY behavior and assertions.
- Public headers under `include/mereader-tui/`; the directory picker is an
  internal TUI concern, not a public library API.
- README, man page, keybinding defaults, colors, and user-visible text.
- Vendored fff code.
- Generic object systems, vtables, inheritance, callbacks, or unrelated
  formatting/refactors.

## Git workflow

- Work on an isolated branch/worktree based on `b6aaf64`.
- Commit one logical refactor with a lowercase conventional message:
  `refactor: extract library folder picker`.
- Do not push or merge unless the operator explicitly instructs it.

## Steps

### Step 1: Add model-level characterization tests

Create `tests/test_library_directory_picker.c` and register it through
`tests/test_support.h`, `tests/test_main.c`, and `Makefile`.

Test the extracted public-internal behavior, not private allocation details:

1. Opening a directory returns sorted child directories, reports whether a
   parent exists, and exposes a `[use this folder]` entry.
2. Moving to the parent preserves selection on the child just left.
3. Selection movement clamps safely for empty, first, last, and page-sized
   moves.
4. A fuzzy query such as `clasic` finds `Classics Collection`, excludes
   unrelated results, and activating the match opens that directory.
5. Searching outside the active catalog index reports scanning without
   blocking, then exposes results after polling completes.
6. Clearing the query releases the transient fff index/results and restores
   ordinary directory entries.
7. Invalid and unreadable paths return the existing typed
   `MereaderTuiError`.
8. Freeing zeroed, partially initialized, and populated state is safe.

Use the existing test support path helpers and EPUB fixtures. Do not add a
second testing framework.

**Verify**: Build the new test with temporary declarations or the new header,
then run `make test`. Before extraction, tests that require the new module may
fail to link; all pre-existing tests must still pass.

### Step 2: Create the cohesive directory-picker module

Create `src/library_directory_picker.h` and
`src/library_directory_picker.c`. Move only the non-ncurses responsibilities:

- owned state and lifecycle;
- directory enumeration and sorting;
- resolved path replacement;
- active-index reuse versus transient asynchronous-index ownership;
- fuzzy result refresh and polling;
- root pseudo-result filtering;
- query/search state transitions;
- entry count and read-only entry labels/paths;
- selection clamping, top-row visibility, parent navigation, and selected
  entry activation.

The module may expose its concrete state in the internal header, matching
`MereaderTuiLibraryShelf`; do not allocate an opaque heap object merely to call
it “OOP.” All functions must use the `mereader_tui_library_directory_picker_`
prefix. Preserve transactional replacement: a failed directory load must
leave the current picker usable. Preserve ownership rules: the picker owns its
transient index and copied results but never closes the active catalog index
passed in for reuse.

Keep ncurses types and calls out of `library_directory_picker.c`. Input
decoding remains in `library_tui.c`; pass semantic operations or updated query
content into the model. Keep application command emission in
`library_tui.c`: activation should return an explicit result describing
“browse completed” versus “use this folder,” with an owned or borrowed path
documented in the header.

Add `src/library_directory_picker.c` to `SOURCES` in `Makefile`.

**Verify**: `make test` → all pre-existing tests and all new model tests pass.

### Step 3: Reduce `library_tui.c` to controller and presentation work

Include `library_directory_picker.h`, replace the local picker struct with the
module state, and delete the moved implementations from `library_tui.c`.
Update the existing drawing functions to use model accessors or documented
read-only fields. Update key handling, the 100 ms poll, setup opening, close,
and cleanup to call module functions.

Do not move these functions out of `library_tui.c`:

- directory row/query/picker drawing;
- curses cursor and color handling;
- key-sequence matching and raw key decoding;
- status/error presentation;
- `MEREADER_TUI_LIBRARY_COMMAND_SET_ROOT` emission.

After the extraction, these forbidden implementation details must no longer
occur in `src/library_tui.c`: `opendir`, `readdir`, `fstatat`,
`mereader_tui_search_index_start`, or
`mereader_tui_search_directories`.

**Verify**:

```sh
rg -n 'opendir|readdir|fstatat|mereader_tui_search_index_start|mereader_tui_search_directories' src/library_tui.c
```

Expected: no matches. Then run `make test`; all tests pass.

### Step 4: Run full behavioral and visual regression gates

Run `make sanitize`. Rebuild normally with `make clean && make -j2 all`, then
run `make installcheck`. Recreate the folder picker in a 72x14 terminal, press
`p`, type `clasic`, and use `agent-browser` to inspect a faithful captured
render. Compare it with the expected behavior in
`tests/test_library.c`: only `Classics Collection/` is ranked, the query line
shows `> clasic`, Enter opens it, a second Enter selects it, and the footer is
not clipped. Confirm the base background remains `#1e1e2e`.

**Verify**: every command exits 0; visual inspection shows no text, layout,
keybinding, or color regression.

## Test plan

- New unit/integration tests in `tests/test_library_directory_picker.c` cover
  lifecycle, sorting, parent selection, bounds, active and asynchronous fff
  search, clearing, activation, and errors.
- Existing PTY tests `library.pty_change_library_folder` and
  `library.pty_fuzzy_library_folder_search` remain unchanged and pass.
- Full native count increases beyond 153; CLI, sanitizer, and install tests
  remain green.

## Done criteria

- [ ] `src/library_directory_picker.c` owns filesystem/search/navigation model
      logic and contains no ncurses calls.
- [ ] `src/library_tui.c` contains no `opendir`, `readdir`, `fstatat`,
      `mereader_tui_search_index_start`, or
      `mereader_tui_search_directories`.
- [ ] User-visible picker behavior, text, keybindings, and colors are
      unchanged.
- [ ] Existing PTY characterization tests are unchanged and pass.
- [ ] `make test`, `make sanitize`, normal rebuild, and `make installcheck`
      all exit 0.
- [ ] Agent-browser visual verification finds no regression.
- [ ] `git diff --check` exits 0.
- [ ] No source/test files outside the in-scope list are modified.
- [ ] `plans/README.md` marks this plan DONE.

## STOP conditions

Stop and report instead of improvising if:

- The folder-picker code has changed from the cited `b6aaf64` state.
- A correct extraction requires changing the public search API or vendored
  fff.
- Preserving the active-index borrowing rule cannot be expressed without
  ambiguous ownership.
- A test or sanitizer failure persists after two cause-directed corrections.
- The refactor changes visible text, keybindings, layout, colors, or folder
  selection semantics.
- The implementation starts introducing generic callbacks, vtables,
  inheritance, or changes to unrelated TUI code.

## Maintenance notes

- Review ownership closely: transient fff indexes are destroyed by the picker;
  borrowed catalog indexes are not.
- Keep UI policy in `library_tui.c`. Future picker search/navigation behavior
  belongs in the extracted module; future drawing/layout changes do not.
- This plan intentionally does not split the rest of `library_tui.c`.
  Additional extraction should follow evidence from churn, coupling, or tests,
  not a line-count target.

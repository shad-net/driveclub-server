# Changelog

## 0.0.7

- Fixed a key issue in accolades

- Real friends support

- Added quick search

- Removed deprecated friends discovery , everything is implemented now.

## 0.0.6

- FriendNames`gains`optional FriendNamesType type = 2 (should fix friends discovery crash)
- StatusResult.retryHint is an enum (should fix clubs discover crash on creating a club)
- Added featured challenges
- Initial accolades support

## 0.0.5

- Fixed: `i_am_special` on challenge boards
  - **The fix:**
    - posts are stored under the caller’s account ID, taken from the OTG token.
    - `select` gives the caller their own row, labelled `i_am_special`, so the game recognises it as theirs.
    - `nearby` centres on the caller.
    - rows that already stored under the literal name are removed when the server starts, because there’s no way to tell whose they were. 
- Club discovery added for debugging
- Friends discovery added for debugging

### 0.0.4, 2026-10-30

- Player stats added

- Fixed duplicated ghosts

- Don't store orphan ghosts

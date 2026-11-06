# Creator Tools Mod

Creator Tools is a small LeviLamina utility mod that speeds up creator-style workflows in Minecraft: switch game modes, reload packs, inspect entities, and keep your chat command history between sessions, all with a few keyboard shortcuts instead of digging through menus.

<!-- TODO: add a screenshot or GIF of the entity debug label here -->

## Controls

| Input                  | Action                                                                                                                                             | Works in                         | Needs                                |
| ---------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------- | ------------------------------------ |
| `G`                    | Cycle game mode: Survival → Creative → Adventure → Spectator                                                                                       | Local worlds and servers         | Local: nothing. Server: operator     |
| `Ctrl` + `R`           | Reload behavior resources without leaving the world (on a remote server it sends `/reload`)                                                        | Local worlds and servers         | Local world, or operator on a server |
| `Ctrl` + `Shift` + `R` | Reload behavior packs (loot tables, entities, ...) with `/reload all`, run inside the live local server; on a remote server it sends `/reload all` | Local worlds (servers: operator) | Local world                          |
| `Shift` + `R`          | Refresh client packs, then reload animations, geometries, textures, fonts and UI                                                                   | Anywhere                         | —                                    |
| `Ctrl` + `Shift` + right-click | Toggle debug features on the targeted entity                                                                                                 | Local worlds only                | Integrated server                    |

Modifiers must match exactly: `G` only fires with no Ctrl, Shift or Alt held, and `Ctrl+R`, `Shift+R` and `Ctrl+Shift+R` never overlap.

Hotkeys only fire when Minecraft is the foreground window and the in-game HUD is the active screen. They are ignored in menus, chat, the inventory, and text-entry screens such as the anvil, signs, books and command blocks, so typing a "g" never changes your game mode.

## Configuration

On first load, Creator Tools creates `config.json` in the mod's config folder with default values. Missing fields are filled with defaults. The file is read when the mod loads, so restart Minecraft after changing it.

- `hotkeys.gamemode` and `hotkeys.reload` accept a letter, digit or function key from `F1` through `F12`.
- `entityDebug.inspectCtrl`, `inspectShift` and `inspectAlt` set the exact modifier combination for the inspect right-click. At least one must be enabled; the default is Ctrl+Shift.
- `entityDebug.hideNameTag`, `entityDebug.labelScale` and `entityDebug.maxBehaviors` control the selected entity's name tag and label.
- `entityDebug.showPath`, `entityDebug.pathHeightOffset`, `entityDebug.pathTargetRadius` and `entityDebug.pathDestRadius` control path display.
- `entityDebug.pathLineColor`, `entityDebug.pathTargetColor` and `entityDebug.pathDestColor` are `[r, g, b, a]` arrays with channel values from 0 to 1.

## Features

### Game mode cycling

In a local world, pressing `G` calls the integrated server's player API directly, so the change is real on the server and needs no cheats. On a remote server there is no API to reach it, so `/gamemode <mode>` is sent as a fallback (operator required). If you press it several times quickly, the cycle keeps advancing instead of repeating one target while the server catches up.

### Fast reloads

- `Ctrl+R` reloads behavior resources on the local server and keeps you in the world.
- `Shift+R` refreshes client packs and reloads client-side assets without leaving the game flow. It reports when it starts and how long it took when it finishes, and reports failures in chat. If a reload gets stuck, pressing `Shift+R` again after 30 seconds starts a fresh one.
- `Ctrl+Shift+R` runs `/reload all` in the local server in-process. It no longer leaves and re-enters the world.

Every reload action posts a chat message, so you don't need to watch the log.

### Persistent command history

Vanilla remembers the commands you have sent only until you close the game. Creator Tools saves them to disk, so after restarting Minecraft you can open chat, press `Up`, and step back through the commands from your previous sessions.

- It uses the game's normal chat history, so there is no new key or screen: `Up` and `Down` work as usual.
- Only slash commands are saved (not ordinary chat messages), the most recent 100 are kept, and a command you repeat is stored once.
- The history is saved shortly after each change, so a crash or force-close doesn't lose your session.
- It is stored as plain text in `command_history.txt` in the mod's data folder. Delete that file to clear the history.
- The first line of the file (`# newest=front` or `# newest=back`) records how the game orders its history. The mod learns this by watching the first couple of commands you send, then reuses it. Don't remove that line.
- It works in every world and on every server, since it is part of the client's chat history and not tied to a world.

### Entity debug

Aim at an entity and `Ctrl`+`Shift`+right-click to show world-space text above it with health, position, properties and behavior-tree details, plus a wireframe of its collision box. Click the same entity again to hide it.

- Behavior details are read from the local world's integrated server.
- The running behavior is highlighted green. It is matched by the actual node, so two behaviors with the same name are not both highlighted. Nested entries are marked with `>` per level, and the running behavior stays visible even when the list is truncated.
- The label is shown only to you, not to LAN guests.
- While an entity is selected, its vanilla name tag is hidden so it doesn't overlap the debug label.
- `Ctrl`+`Shift`+right-click on an entity is consumed by the mod and won't also trigger the normal interaction (trading, mounting, etc.). A plain `Ctrl`+right-click, including when Ctrl is bound to sneak, is not affected.

#### Path visualization

While an entity is selected and it has an active navigation path, the path is drawn in the world:

- An orange line runs from the entity along the remaining path nodes. It shortens as the entity advances and disappears when the path ends.
- A small yellow sphere marks the node the entity is walking to right now.
- A larger red sphere marks the final node of the path.
- Up to 48 segments are drawn. If the path is longer, the red destination marker is left out, since the last drawn node isn't the real end.
- Entities with no navigation (players, items, most non-mobs) simply show no path.

Colors, marker sizes and the on/off switch are configurable in `config.json`.

## Known limitations

- Windows only (keyboard state and window focus use the Win32 API).
- On remote servers `G`, `Ctrl+R` and `Ctrl+Shift+R` fall back to sending `/gamemode`, `/reload` and `/reload all`, so they need operator permission. Local worlds use direct server calls and need no cheats.
- The entity debug label, behavior-tree details and path visualization need a local world with an integrated server.
- Only one `Shift+R` client reload can run at a time. Extra presses are ignored with a chat message.
- The saved command history is shared across all worlds and servers, is stored unencrypted on disk, and holds slash commands only. Avoid typing secrets (such as passwords in commands) if that matters to you; `kOnlyCommands` in `CommandHistory.cpp` controls what is saved.

## Troubleshooting

**Hotkeys do nothing in-game.** The mod decides "gameplay" by checking that the top screen is named `hud_screen` (see `kGameplayScreenName` in `GameplayState.h`). With debug logging enabled, the mod logs the top screen name whenever it ignores a matching key press, so you can see what your version calls it.

**Command history doesn't come back after a restart.** Check that `command_history.txt` exists in the mod's data folder and contains your commands. On startup the log should say `Restored N command(s)`. If the history looks reversed after a restart, delete the file, send two or three commands, and restart again. The log line `Chat history order learned` confirms the order was detected.

<!-- TODO: add Requirements (Minecraft / LeviLamina versions), Install and Build sections -->

## Project status

This mod is built for creator-focused testing and editing workflows inside LeviLamina-based Minecraft environments.

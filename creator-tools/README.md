# Creator Tools Mod

Creator Tools is a small LeviLamina utility mod that speeds up creator-style workflows in Minecraft: switch game modes, reload packs, and inspect entities with a few keyboard shortcuts instead of digging through menus.

<!-- TODO: add a screenshot or GIF of the entity debug label here -->

## Controls

| Input                  | Action                                                                                                                                             | Works in                         | Needs                                |
| ---------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------- | ------------------------------------ |
| `G`                    | Cycle game mode: Survival → Creative → Adventure → Spectator                                                                                       | Local worlds and servers         | Local: nothing. Server: operator     |
| `Ctrl` + `R`           | Reload behavior resources without leaving the world (on a remote server it sends `/reload`)                                                        | Local worlds and servers         | Local world, or operator on a server |
| `Ctrl` + `Shift` + `R` | Reload behavior packs (loot tables, entities, ...) with `/reload all`, run inside the live local server; on a remote server it sends `/reload all` | Local worlds (servers: operator) | Local world                          |
| `Shift` + `R`          | Refresh client packs, then reload animations, geometries, textures, fonts and UI                                                                   | Anywhere                         | —                                    |
| `Ctrl` + right-click   | Toggle the debug label on the targeted entity                                                                                                      | Local worlds only                | Integrated server                    |

Modifiers must match exactly: `G` only fires with no Ctrl, Shift or Alt held, and `Ctrl+R`, `Shift+R` and `Ctrl+Shift+R` never overlap.

Hotkeys only fire when Minecraft is the foreground window and the in-game HUD is the active screen. They are ignored in menus, chat, the inventory, and text-entry screens such as the anvil, signs, books and command blocks, so typing a "g" never changes your game mode.

## Features

### Game mode cycling

In a local world, pressing `G` calls the integrated server's player API directly, so the change is real on the server and needs no cheats. On a remote server there is no API to reach it, so `/gamemode <mode>` is sent as a fallback (operator required). If you press it several times quickly, the cycle keeps advancing instead of repeating one target while the server catches up.

### Fast reloads

- `Ctrl+R` reloads behavior resources on the local server and keeps you in the world.
- `Shift+R` refreshes client packs and reloads client-side assets without leaving the game flow. It reports when it starts and how long it took when it finishes, and reports failures in chat. If a reload gets stuck, pressing `Shift+R` again after 30 seconds starts a fresh one.
- `Ctrl+Shift+R` runs `/reload all` in the local server in-process. It no longer leaves and re-enters the world.

Every reload action posts a chat message, so you don't need to watch the log.

### Entity debug label

Aim at an entity and `Ctrl`+right-click to show world-space text above it with health, position and behavior-tree details, plus a wireframe of its collision box. Click the same entity again to hide it.

- Behavior details are read from the local world's integrated server.
- The running behavior is highlighted green. It is matched by the actual node, so two behaviors with the same name are not both highlighted. Nested entries are marked with `>` per level, and the running behavior stays visible even when the list is truncated.
- The label is shown only to you, not to LAN guests.
- `Ctrl`+right-click on an entity is consumed by the mod and won't also trigger the normal interaction (trading, mounting, etc.).

## Known limitations

- Windows only (keyboard state and window focus use the Win32 API).
- On remote servers `G`, `Ctrl+R` and `Ctrl+Shift+R` fall back to sending `/gamemode`, `/reload` and `/reload all`, so they need operator permission. Local worlds use direct server calls and need no cheats.
- The entity debug label and behavior-tree details need a local world with an integrated server.
- Only one `Shift+R` client reload can run at a time. Extra presses are ignored with a chat message.

## Troubleshooting

**Hotkeys do nothing in-game.** The mod decides "gameplay" by checking that the top screen is named `hud_screen` (see `kGameplayScreenName` in `GameplayState.h`). With debug logging enabled, the mod logs the top screen name whenever it ignores a matching key press, so you can see what your version calls it.

<!-- TODO: add Requirements (Minecraft / LeviLamina versions), Install and Build sections -->

## Project status

This mod is built for creator-focused testing and editing workflows inside LeviLamina-based Minecraft environments.

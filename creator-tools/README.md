# Creator Tools Mod

Creator Tools is a small LeviLamina utility mod designed to speed up creator-style gameplay workflows in Minecraft. It adds fast keyboard shortcuts for switching game modes and reloading client-side or server-side resources without needing to dig through menus.

## Overview

The mod is intentionally focused and lightweight:

- G: (no modifier keys held) cycles the local player's game mode in order: Survival -> Creative -> Adventure -> Spectator
- Ctrl + R: sends `/reload` to the current world or server
- Ctrl + Shift + R: reloads behavior resources on a local world and automatically leaves and re-enters it (local worlds only; gives up after 30 seconds if the world doesn't shut down)
- Shift + R: refreshes client packs, then reloads animations, geometries, textures, fonts, and UI resources
- Ctrl + right-click: toggles a world-space debug label for the targeted entity in local worlds

These actions are only triggered when Minecraft is the active foreground window and the player is in gameplay, which helps avoid accidental input while menus or chat are open.

## Features

### Game mode cycling

Pressing G asks the server to switch you to the next mode in the standard cycle by sending `/gamemode <mode>`, so the change is real on the server (not just on your client). This is useful for testing builds, creative movement, and quick world editing.

### Fast reloads

- Ctrl + R requests a server-side behavior-resource reload for a local world
- Shift + R refreshes client packs and reloads client-side assets without leaving the game flow

This makes the mod helpful for creators iterating on map layouts, tests, and visual adjustments.

### Entity debug label

Aim at an entity and Ctrl + right-click to show world-space text above it with health, position, and behavior-tree details, plus a wireframe of its collision box.

Behavior details are read from the local world's integrated server, with the currently running behavior highlighted green.

Reload actions show a chat message when they start or finish (Shift + R includes how long it took), so you don't need to watch the log. Ctrl + right-click on an entity is consumed by the mod and won't also trigger the normal interaction (trading, mounting, etc.).

## Known limitations

- Windows only (keyboard state and window focus use the Win32 API).
- G and Ctrl + R run server commands (`/gamemode`, `/reload`), so they need cheats enabled in a local world or operator permission on a server. Without permission the server replies with an error in chat.
- The entity debug label and behavior-tree details need a local world with an integrated server.
- Only one Shift + R client reload and one Ctrl + Shift + R re-entry can run at a time; extra presses are ignored with a log message.

## Project status

This mod is built for creator-focused testing and editing workflows inside LeviLamina-based Minecraft environments.

# Creator Tools Mod

Creator Tools is a small LeviLamina utility mod designed to speed up creator-style gameplay workflows in Minecraft. It adds fast keyboard shortcuts for switching game modes and reloading client-side or server-side resources without needing to dig through menus.

## Overview

The mod is intentionally focused and lightweight:

- G: cycles the local player's game mode in order: Survival -> Creative -> Adventure -> Spectator
- Ctrl + R: requests a behavior-resource reload on a local world, or sends `/reload` to a remote server
- Shift + R: refreshes client packs, then reloads animations, geometries, textures, and UI resources
- Ctrl + right-click: toggles a floating debug label for the targeted entity in local worlds

These actions are only triggered when Minecraft is the active foreground window and the player is in gameplay, which helps avoid accidental input while menus or chat are open.

## Features

### Game mode cycling

Pressing G changes the active local player's game mode to the next mode in the standard cycle. This is useful for testing builds, creative movement, and quick world editing.

### Fast reloads

- Ctrl + R requests a server-side behavior-resource reload for a local world
- Ctrl + R sends `/reload` when connected to a remote server
- Shift + R refreshes client packs and reloads client-side assets without leaving the game flow

This makes the mod helpful for creators iterating on map layouts, tests, and visual adjustments.

### Entity debug label

Aim at an entity and Ctrl + right-click to show a floating label above it with health, position, velocity, and behavior-tree details, plus a wireframe of its collision box. The label follows the entity without changing its name tag. Ctrl + right-click the selected entity again to hide it, or select a different entity to switch the label.

Behavior details are read from the local world's integrated server; they are unavailable for entities on remote servers.

## Project status

This mod is built for creator-focused testing and editing workflows inside LeviLamina-based Minecraft environments.

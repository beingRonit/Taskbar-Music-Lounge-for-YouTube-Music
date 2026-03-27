# Taskbar Music Lounge — YouTube Music

A Spotify-style floating miniplayer for YouTube Music, displayed on the Windows taskbar.

## Features

- **Album art** with rounded corners and smooth crossfade transitions
- **Title & artist** display with auto-scrolling for long titles
- **Transport controls**: Previous / Play-Pause / Next
- **Real-time progress bar** with high-resolution interpolation
- **Click-to-seek** on the progress bar
- **Auto light/dark theme** based on Windows settings
- **Smart hiding** when YouTube Music stops or goes fullscreen
- **Click-through** when idle (taskbar stays usable)
- **End-of-track fade** effect
- **Fade-in animation** on new track start
- **Adaptive polling** for optimized CPU usage

## Performance Optimizations

- **Adaptive polling**: 250ms when playing, 1000ms when paused
- **High-resolution interpolation**: Uses `std::chrono::steady_clock` for smooth progress
- **Resource caching**: Fonts and brushes cached to reduce GDI+ allocations
- **Skip hidden redraws**: No rendering when window is hidden

## Requirements

- Windows 10/11
- [Windhawk](https://windhawk.net/) mod engine
- Disable Windows Taskbar Widgets for best compatibility

## Installation

1. Install Windhawk
2. Go to **Settings → Develop a mod**
3. Create new mod with ID: `taskbar-music-lounge-ytm`
4. Paste the contents of `mod.wh.cpp` into the mod editor
5. Build and enable the mod

## Settings

| Setting | Default | Description |
|---------|---------|-------------|
| Panel Width | 500 | Width of the miniplayer |
| Panel Height | 130 | Height of the miniplayer |
| Font Size | 18 | Title font size |
| Button Scale | 2.0 | Button scaling factor |
| Hide Fullscreen | false | Hide when apps are fullscreen |
| Idle Timeout | 25 | Seconds before hiding when paused (0 = off) |
| Offset X | 1417 | X position offset from taskbar left |
| Offset Y | -95 | Y position offset from taskbar center |
| Auto Theme | true | Follow Windows light/dark mode |
| Bg Opacity | 128 | Acrylic background opacity (0-255) |
| End Fade Threshold | 4 | Seconds before track end to start fade (0 = off) |

## Layout

```
┌──────────────────────────────────────┐
│ [Album]  Song Title                   │
│          Artist Name                   │
│          [◄◄] [▶/❚❚] [►►]            │
│  ████████░░░░░░░░░░░░░  1:23 / 3:45 │
└──────────────────────────────────────┘
```

## Controls

- **Click progress bar** to seek
- **Hover progress bar** to see thumb handle
- **Mouse wheel** to adjust system volume
- **Spacebar** to play/pause
- **←/→ keys** for previous/next track

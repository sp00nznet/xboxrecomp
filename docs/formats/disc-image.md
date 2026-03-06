# Xbox Disc Images

This document covers the Xbox disc image formats and how to extract game files for use with the recompilation toolkit.

## XISO Format

Xbox games ship on proprietary DVD discs that use the **XDVDFS** (Xbox DVD File System) format. Disc images are typically distributed as `.iso` or `.xiso` files.

### Structure

- The XDVDFS volume descriptor is located at sector 32 of the disc image (byte offset 0x10000)
- The file system is a simple tree of directory entries with no journaling or permissions
- Maximum file name length is 255 characters (ASCII only)
- Maximum file size is 4 GB (limited by 32-bit size fields)
- `default.xbe` is always at the root of the file system

### Key Properties

| Property | Value |
|----------|-------|
| Sector size | 2048 bytes |
| Volume descriptor | Sector 32 (offset 0x10000) |
| Magic | "MICROSOFT*XBOX*MEDIA" at descriptor + 0x00 |
| Root directory | Offset and size at descriptor + 0x14 |
| Timestamp | At descriptor + 0x1C (FILETIME format) |

## Extraction Tools

### xdvdfs (Recommended)

A Rust-based tool for reading and extracting Xbox disc images.

- Repository: https://github.com/antangelo/xdvdfs
- Install: `cargo install xdvdfs-cli`
- Extract: `xdvdfs unpack game.iso output_dir/`
- List: `xdvdfs ls game.iso`

### extract-xiso

The classic C-based Xbox ISO extraction tool.

- Repository: https://github.com/XboxDev/extract-xiso
- Extract: `extract-xiso -x game.iso`
- Creates a directory named after the ISO file

### FTP from a Modded Xbox

If you have a modded original Xbox:
1. Install a dashboard with FTP support (UnleashX, XBMC, etc.)
2. FTP to the Xbox's IP address (default user/pass varies by dashboard)
3. Navigate to `E:\Games\` or `F:\Games\` (where games are installed)
4. Download the entire game directory

## Disc Layout

A typical Xbox game disc contains:

```
/
  default.xbe          - Main executable (always present)
  default.xip          - Xbox dashboard resources (optional)
  game_data/           - Game-specific data files
  media/               - Video/audio content
  Tracks/              - Track/level data (racing games)
  ...                  - Varies per game
```

### Multiple Disc Layers

Some Xbox games use a dual-layer DVD (DVD-9), providing up to ~7.9 GB of storage versus ~4.7 GB for single-layer (DVD-5). The file system spans both layers transparently.

### Title Updates

Title updates (patches) are stored on the Xbox hard drive at:
- `E:\TDATA\<TitleID>\$u\` (user data area)
- `Y:\<TitleID>\` (cache partition)

These updates modify or add files to the base game. For recompilation purposes, you generally want the unpatched base game unless the update fixes a critical bug.

## Practical Notes

1. **File paths**: Xbox games use backslash paths internally (`D:\Tracks\EU\C1_V1\streamed.dat`). The `D:\` drive maps to the DVD root. When extracting, preserve the directory structure.

2. **Case sensitivity**: XDVDFS is case-insensitive. Games may reference files with inconsistent casing. Your host file system layer should handle case-insensitive lookups.

3. **File alignment**: Some game files are aligned to sector boundaries on disc for streaming performance. After extraction to a hard drive, alignment is irrelevant but file offsets referenced in game data structures remain valid.

4. **Redump verification**: For archival-quality disc images, use the Redump database (http://redump.org/) to verify your dump matches known-good checksums. This ensures the image is complete and uncorrupted.

5. **Legal considerations**: You should only use disc images of games you legally own. The recompilation toolkit is designed for personal preservation and research purposes.

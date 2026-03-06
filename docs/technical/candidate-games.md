# Candidate Games for Xbox Static Recompilation

This document provides guidance on selecting Xbox games as targets for the static recompilation toolkit. Games are tiered by estimated difficulty based on their technical characteristics.

## Selection Criteria

When evaluating a game for static recompilation, consider:

1. **Code size**: Smaller .text sections mean fewer functions to recompile and debug
2. **Engine**: Well-known engines (RenderWare, Unreal, etc.) have documented behavior
3. **XDK version**: Newer XDK versions use more complex library code
4. **Threading**: Single-threaded games are much easier than multi-threaded ones
5. **GPU usage**: Games that use D3D8 "normally" are easier than those with custom NV2A push buffer code
6. **Network**: Offline-only games avoid Xbox Live complexity entirely
7. **Dynamic code**: Games with no self-modifying code or dynamic code generation are strongly preferred

## Proven Target

### Burnout 3: Takedown (2004)

The first game successfully targeted by this toolkit. Key characteristics:
- **Engine**: Criterion's custom RenderWare fork (~3.7), statically linked
- **XDK**: 5849 (late-era, complex but well-documented)
- **Code size**: 2.73 MB .text section (~22,000 functions)
- **Kernel imports**: 147 functions
- **Libraries**: 11 statically linked XDK libs (D3D8LTCG, DSOUND, XMV, XONLINE, etc.)

Current status: boots, loads game data, runs gameplay loop with a custom rendering frontend. See the [burnout3 repository](https://github.com/sp00nznet/burnout3) for the full implementation.

## Tier 1: Good First Targets

These games have characteristics that make them relatively straightforward for static recompilation.

### Characteristics
- Small to medium code size (< 2 MB .text)
- Well-known engine with existing documentation
- Minimal or no Xbox Live requirements
- Standard D3D8 usage (no raw NV2A push buffer manipulation)
- Few or no demand-loaded XBE sections

### Example Candidates

**RenderWare Engine Games** (same engine family as the proven target):
- Grand Theft Auto III / Vice City / San Andreas
- Burnout 1 / Burnout 2: Point of Impact
- Tony Hawk's Pro Skater 2x / 3 / 4

**Simple Architecture Games**:
- Halo: Combat Evolved (well-documented, large modding community)
- Jet Set Radio Future (Sega/Smilebit, relatively simple rendering)
- Panzer Dragoon Orta (Sega/Smilebit)

## Tier 2: Moderate Difficulty

### Characteristics
- Medium code size (2-4 MB .text)
- Engine may be less documented
- May use some Xbox-specific features (XMV video, custom audio)
- Possible Xbox Live features (but offline play works)

### Example Candidates
- Fable (Lionhead, custom engine)
- Star Wars: Knights of the Old Republic (BioWare/Odyssey engine)
- Ninja Gaiden (Team Ninja, complex rendering)
- Project Gotham Racing 1/2 (Bizarre Creations)

## Tier 3: Challenging

### Characteristics
- Large code size (> 4 MB .text)
- Heavy use of Xbox-specific hardware features
- Complex multi-threading model
- Custom NV2A push buffer code (bypasses D3D8)
- Significant Xbox Live integration

### Example Candidates
- Halo 2 (complex engine, multi-threaded, custom rendering)
- Dead or Alive 3 / Xtreme Beach Volleyball (Team Ninja, NV2A-heavy)
- Conker: Live & Reloaded (Rare, complex shaders)
- Any game requiring Xbox Live for core functionality

## Evaluation Checklist

Before committing to a game, perform this analysis:

### 1. XBE Analysis
```
- Parse the XBE header to get section layout
- Count kernel imports (< 150 is typical)
- Check XDK version (library build numbers)
- List statically linked libraries
- Note any demand-loaded sections (XeLoadSection calls)
```

### 2. Code Complexity Assessment
```
- Measure .text section size
- Estimate function count (typical: 1 function per ~130 bytes)
- Check for x87 FPU vs SSE usage
- Look for self-modifying code patterns
- Identify inline assembly or hand-optimized routines
```

### 3. Data Format Survey
```
- Identify asset formats (textures, models, audio, video)
- Check for standard formats (DDS, WAV) vs proprietary
- Note any encrypted or compressed assets
- Estimate total data size
```

### 4. Runtime Behavior
```
- Does the game boot to a menu without disc-specific checks?
- How many threads does it create at startup?
- Does it require Xbox Live authentication?
- Does it use XMV video playback?
- Does it write to NV2A registers directly?
```

## General Advice

1. **Start with what you know**: If you're familiar with a game's engine from modding or reverse engineering, that knowledge transfers directly.

2. **RenderWare games are a natural second target**: The kernel layer, memory layout, and D3D8 abstraction from Burnout 3 are directly reusable. Only the game-specific code and asset formats change.

3. **Avoid launch titles**: Early Xbox games (2001-2002) sometimes use non-standard XDK patterns or debug-era libraries that complicate analysis.

4. **Check the modding community**: Games with active modding scenes often have partially documented file formats, memory layouts, and function signatures. This saves enormous amounts of reverse engineering time.

5. **Test incrementally**: Get the game to a black screen with no crashes before attempting rendering. Get rendering before attempting audio. Get gameplay before attempting saving/loading.

6. **Keep the original disc image intact**: Your recompiled executable reads game assets directly from the extracted disc contents. Never modify the original data files.

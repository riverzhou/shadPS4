# GT7: clamp HDR history to remove the white bloom

## Summary

GT7's Music Rally race reuses RGBA16F history targets for temporal composition.
The targets accumulate values far above 1.0, and the composite pass feeds them
back into the next frame. The result is a giant white bloom that covers the
road, cockpit and mirror.

This change adds a host compute pass that rewrites each history image as

```glsl
color.rgb = min(color.rgb * exposure, vec3(max_value));
```

with optional highlight-only compression above `threshold`. It is opt-in.

## Usage

```text
SHADPS4_GPU_CLAMP_HISTORY=1
SHADPS4_CLAMP_SHADER=1
# optional
SHADPS4_CLAMP_EXPOSURE=1.0
SHADPS4_CLAMP_MAX=1.0
SHADPS4_CLAMP_THRESHOLD=0.0
```

The launcher enables the first two by default.

## Evidence

Before / after, same race, 1920x1080:

- `docs/pr/before.jpg`
- `docs/pr/after.jpg`

Logs from the target machine:

```text
CLEARHDR addr=0x1007600000 frame=...
CLEARHDR addr=0x10085f0000 frame=...
SPV output patched to sample 499 via function variable
```

## Testing

- Patched build: `D:/GitHub/shadPS4/Build/clean/shadps4.exe`
- Navigation: `python tools/gt7_auto.py --full --stop-at-race`
- Reached `real race started`, `laps=1/1`, held R2 for 120 s.
- No device-lost, no validation errors in the race.
- Driver: NVIDIA 617.14, 1920x1080.

## Limitations

- This is a GT7-oriented experimental feature. The history addresses
  (`0x1007600000`, `0x10085f0000`) are currently hardcoded and gated by an
  environment variable; it is not yet a generic upstream fix.
- The white bloom is removed, but the road still reads brighter than the
  retail reference, and the mirror is still empty. Those are separate issues.
- The compute pass runs every fourth frame; clamping every frame is possible
  but was not benchmarked.

## Next steps

- Make the history target selection generic (image usage/format instead of
  guest addresses).
- Add a proper configuration option instead of environment variables.

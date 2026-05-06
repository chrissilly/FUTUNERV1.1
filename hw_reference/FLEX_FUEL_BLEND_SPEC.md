# FUTUNER Flex Fuel Blend System Specification

## Overview
Two calibration files (Tune A = 91 octane pump gas, Tune B = E85) are loaded via XDF+BIN pairs. Performance maps are blended between A and B based on real-time ethanol content from a physical fuel line sensor.

The ECU natively adjusts fuel mass when we write the ethanol % value to its ethanol address — we do NOT blend fuel quantity maps. We only blend performance maps (boost, timing, limiters).

## Blend Curves
Two user-definable curves control the blending:

### 1. Ethanol-Load Curve
- X axis: Ethanol % (0-100)
- Y axis: Blend factor (0-100%)
- Used for: boost target, fuel pressure, torque limiters, load-related maps
- User defines breakpoints (e.g. 10 points)

### 2. Ethanol-Ignition Curve
- X axis: Ethanol % (0-100)
- Y axis: Blend factor (0-100%)
- Used for: ignition timing, knock threshold maps
- Separate shape from load curve (timing can be more aggressive at lower ethanol)
- User defines breakpoints

### Breakpoints
- User can add/remove/drag breakpoints on each curve
- Interpolation between breakpoints is linear
- Example breakpoints for load curve:
  - 0% eth → 0% blend (100% Tune A)
  - 20% eth → 10% blend
  - 40% eth → 35% blend
  - 60% eth → 65% blend
  - 85% eth → 100% blend (100% Tune B)

## Per-Map Assignment
Each map from the XDF is assigned one of:
- **None** — always uses Tune A value (no blending)
- **Ethanol-Load** — blended using the load curve
- **Ethanol-Ignition** — blended using the ignition curve

## Blend Formula
```
curve_factor = interpolate_curve(current_ethanol%, selected_curve_breakpoints)
blended_value = TuneA_value + (TuneB_value - TuneA_value) * (curve_factor / 100)
```

## ECU Native Fueling
- Writing ethanol % to `InjSys_ratEthPrtnBascFu` address causes ECU to auto-adjust fuel mass
- This is a physical sensor reading from the fuel line — real hardware value
- Fuel maps do NOT need blending — ECU handles it internally

## Data Flow
1. Physical ethanol sensor → ECU reads ethanol %
2. FUTUNER reads ethanol % via UDS logger (InjSys_ratEthPrtnBascFu)
3. Look up load curve factor and ignition curve factor at current ethanol %
4. For each enabled map: calculate blended cell values
5. Write blended values to ECU RAM via write_ecu (64-byte chunks max)
6. Rate-limited: only write when ethanol % changes by >1% or on manual trigger

## Safety
- Rate-limit ECU writes (don't spam on tiny ethanol fluctuations)
- Manual trigger button to force a blend update
- Show what's currently in ECU vs what blend calculates
- Revert to Tune A button (safe fallback)

# Mac Fan Controller 1.0.3

## Highlights

- SwiftUI app with a slider from `AUTO` to maximum fan target.
- SMC helper installed as a privileged local tool.
- Shows fan RPM, min/max/target RPM, temperature keys, power-related keys, and status messages.
- Supports Apple Silicon per-fan mode keys (`F0Md`, `F1Md`, etc.) and legacy Intel-style fan control.
- Restores `AUTO` on app quit by default.
- Auto-detects model identifier, platform, fan count, and writable control strategy.
- Expands fan probing to all four-character fan indexes from `F0` through `F9`.
- Publishes the release bundle as `MacFanController-<version>-Installer.zip`.
- Keeps the public source tree focused on runtime, build, packaging, and user documentation files.

## Safety

- `AUTO` returns control to macOS.
- The app keeps failed operation messages visible until cleared.
- The helper refuses write commands unless installed as the privileged helper.
- The helper refuses guessed target speeds when min/max RPM are unavailable.

## Known Limits

- The unsigned local build may be blocked by Gatekeeper until installed via Terminal or approved by the user.
- SMC keys vary by Mac model. Unsupported machines may read sensors but reject fan control writes.
- Public releases are unsigned unless a distributor signs and notarizes the package separately.

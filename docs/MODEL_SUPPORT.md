# Model Support

Mac Fan Controller is designed for fan-equipped Macs from 2015 onward.

Instead of maintaining a brittle hard-coded model table, the helper detects the
current Mac at runtime:

1. Reads `hw.model` and `hw.machine`.
2. Reads `FNum` to discover fan count.
3. Probes per-fan keys such as `F0Ac`, `F0Mn`, `F0Mx`, `F0Tg`.
4. Probes Apple Silicon mode keys such as `F0Md` / `F0md`.
5. Falls back to legacy Intel `FS!` manual-mode control when available.
6. Marks the machine as read-only when fans are visible but no known write mode
   is accepted.

## Expected Coverage

Supported when the machine exposes compatible SMC fan keys:

- MacBook Pro, 2015 to present
- Intel MacBook Air models with fans, 2015 to 2020
- Mac mini, 2018 to present
- iMac, 2015 to present
- iMac Pro, 2017
- Mac Studio, 2022 to present
- Mac Pro, 2019 to present

Intentionally not controllable:

- 12-inch MacBook models, because they are fanless
- Apple Silicon MacBook Air models, because they are fanless
- Any Mac where `FNum` reports zero fans

## Strategy Labels

The app shows the selected strategy in the control panel:

- `per-fan mode keys`: Apple Silicon-style `F0Md`, `F1Md`, etc.
- `legacy FS! mask`: Intel-style global fan manual mask.
- `mixed per-fan mode + legacy FS!`: unusual hybrid exposure.
- `read-only sensors`: fan data can be read, but writes are not available.
- `no fan detected`: the machine appears fanless.

## Notes

SMC is not a public stable API. Different macOS releases and Mac models may
change which keys are writable. The app refuses to guess when required min/max
RPM values are unavailable, and it restores `AUTO` on exit by default.

Apple official model identification pages:

- MacBook Pro: https://support.apple.com/en-us/108052
- MacBook Air: https://support.apple.com/en-us/102869
- Mac mini: https://support.apple.com/en-us/102852
- iMac: https://support.apple.com/en-us/108054
- Mac Studio: https://support.apple.com/en-us/102231
- Mac Pro: https://support.apple.com/en-us/102887

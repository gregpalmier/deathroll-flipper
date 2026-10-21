# Death Roll — Flipper Zero Game

A two-player "Death Roll" dice game for the Flipper Zero. Players take turns rolling an ever-shrinking number range — the player who rolls a **1** loses!

## Building with ufbt

### Prerequisites

- Install [ufbt](https://github.com/flipperdevices/flipperzero-ufbt):

```bash
python3 -m pip install --upgrade ufbt
```

### Build

Update the SDK to the latest official release, then build:

```bash
ufbt update
ufbt
```

The compiled `.fap` file will be placed in the `dist/` directory.

### Install to Flipper Zero

Connect your Flipper Zero via USB, then:

```bash
ufbt launch
```

This builds the app and immediately installs and runs it on the connected Flipper.

## Disclosure

This project was made with the assistance of AI.

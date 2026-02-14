# phi-adapter-hue

## Overview

Integrates Philips Hue Bridge devices with phi-core.

## Supported Devices / Systems

- Philips Hue Bridge (local LAN API)
- Devices exposed by the bridge

## Cloud Functionality

- Cloud required: `optional`
- Primary operation is local via bridge; optional cloud-assisted discovery may be used

## Known Issues

- Pairing requires pressing the physical bridge link button in time.

## License

See `LICENSE`.

---

## Developer Documentation

### Purpose

Provides a Hue adapter plugin for bridge discovery, pairing, and runtime control.

### Features

- mDNS + manual bridge discovery paths
- Pairing/token workflow
- Logging category `phi-core.adapters.hue`

### Runtime Requirements

- phi-core with plugin loading enabled
- Network access to Hue Bridge

### Build Requirements

- `cmake`
- Qt6 modules: `Core`, `Network`
- `phi-adapter-api` (local checkout or installed package)

### Configuration

- Config file deployed with plugin: `hue-config.json`
- Bridge host/token details are managed through phi-core adapter configuration

### Build

```bash
cmake -S . -B build
cmake --build build --parallel
```

### Installation

- Build output: `build/plugins/adapters/libphi_adapter_hue.so`
- Deploy to: `/opt/phi/plugins/adapters/`
- Also deploy: `hue-config.json`

### Troubleshooting

- Error: pairing/token request rejected
- Cause: link button not pressed or stale bridge/token state
- Fix: trigger bridge link mode and retry

### Maintainers

- Phi Systems Tech team

### Issue Tracker

- https://github.com/phi-systems-tech/phi-adapter-hue/issues

### Releases / Changelog

- https://github.com/phi-systems-tech/phi-adapter-hue/releases
- https://github.com/phi-systems-tech/phi-adapter-hue/tags

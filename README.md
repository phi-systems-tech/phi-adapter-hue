# phi-adapter-hue

## Overview

Integrates Philips Hue Bridge devices with phi-core via IPC sidecar.

## Supported Devices / Systems

- Philips Hue Bridge (local LAN API)
- Hue lights exposed by bridge resource API v2

## Cloud Functionality

- Cloud required: `no`
- Local bridge integration only

## Known Issues

- Pairing still requires pressing the physical bridge link button.
- Color state roundtrip is limited (write supported; live color readback depends on scalar transport).

## License

See `LICENSE`.

---

## Developer Documentation

### Purpose

Provides a Philips Hue IPC sidecar adapter using `phi-adapter-sdk`.

### Features

- IPC sidecar executable (`phi_adapter_hue_ipc`)
- Descriptor-driven config schema (`configSchema`) sent during bootstrap
- Factory action `probe` (`Test connection`) with pairing support
- Instance action `startDeviceDiscovery`
- Poll-based v2 snapshot sync (`device`, `light`, `room`, `zone`, `scene`)

### Runtime Requirements

- phi-core with IPC adapter runtime enabled
- Network access to Hue Bridge endpoint

### Build Requirements

- `cmake`
- Qt6 modules: `Core`, `Network`
- `phi-adapter-sdk` (local checkout in `../phi-adapter-sdk` or installed package)

### Configuration

Adapter settings are configured through phi-core:

- `host`
- `port`
- `useTls`
- `appKey`
- `pollIntervalMs`
- `retryIntervalMs`

### Build

```bash
cmake -S . -B build
cmake --build build --parallel
```

### Installation

- Build output: `build/plugins/adapters/phi_adapter_hue_ipc`
- Deploy to: `/opt/phi/plugins/adapters/`

### Troubleshooting

- Error: `Press the link button on the Hue bridge, then retry.`
- Cause: bridge not in pairing mode
- Fix: press bridge link button and run `Test connection` again

### Maintainers

- Phi Systems Tech team

### Issue Tracker

- https://github.com/phi-systems-tech/phi-adapter-hue/issues

### Releases / Changelog

- https://github.com/phi-systems-tech/phi-adapter-hue/releases
- https://github.com/phi-systems-tech/phi-adapter-hue/tags

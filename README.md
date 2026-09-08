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

## License

See `LICENSE`.

---

## Developer Documentation

### Purpose

Provides a Philips Hue IPC sidecar adapter using `phi-adapter-sdk`.

### Features

- IPC sidecar executable (`phi_adapter_hue_ipc`), one loop, no Qt
- Descriptor-driven config schema (`configSchema`) sent during bootstrap
- Factory action `probe` (`Test connection`) with pairing support
- Instance action `startDeviceDiscovery`
- Event stream (`/eventstream/clip/v2`) as the source of state; a poll of
  every CLIP v2 resource type as the safety net (once a minute while the
  stream is up)
- Button events through the SDK's `ButtonPresses`: a click is a single or
  a double, never both, decided 500 ms after the release

### Layout

| File | What it decides |
|---|---|
| `hue_settings` | where the bridge is, how it is spoken to, and how its certificate is verified |
| `hue_model` | a bridge's answers into devices, channels, rooms, zones and scenes; a channel write into a body; what an event carries |
| `hue_events` | the server-sent event stream, taken apart |
| `hue_probe` | the factory's "Test connection" and pairing |
| `hue_instance` | the AdapterInstance: the poll, the stream, everything that talks to phi-core |
| `hue_schema` | name, icon, capabilities, configuration form |

### How the bridge's certificate is verified

Every Hue bridge presents a certificate signed by Signify's own root
(`CN=root-bridge`) with the bridge id as its common name. The root is
installed with the package (`huebridge_cacert.pem`) and named to the adapter;
the instance verifies the chain against it and expects the bridge id (the
adapter's external id) as the certificate's name, even though the bridge is
dialled by IP. A probe, which does not know the id yet, verifies the chain
alone. The previous adapter verified nothing (`VerifyNone`). A CA of one's
own goes into the meta as `tlsCaFile`.

### A poll that could not fetch everything

The poll asks for fourteen resource types, one request after another.
`device` and `light` are required; a poll without them is no poll. Any other
type that could not be fetched is logged, once per poll, and its channels are
carried over from the last poll that could fetch it. It used to be treated as
empty, which told phi-core the device had no buttons; phi-core removed them and
the next poll created them again with new ids - the button that vanished from
the app and came back.

### What is announced

A device is announced when it is new or when its definition changed (name,
class, channels, effects); a value change is reported as a value, and only
when it differs from the last one reported. Rooms, zones and scenes the same.

### Runtime Requirements

- phi-core with IPC adapter runtime enabled
- Network access to Hue Bridge endpoint

### Build Requirements

- `cmake`
- `nlohmann-json3-dev`
- `phi-adapter-sdk` >= 0.13.0 (local checkout in `../phi-adapter-sdk` or installed package)

No Qt.

### Configuration

Adapter settings are configured through phi-core:

- `host`
- `port`
- `useTls`
- `appKey`
- `pollIntervalMs`
- `retryIntervalMs`
- `tlsCaFile` (optional; replaces the bundled Signify root)

### Build

```bash
cmake -S . -B build -G Ninja
cmake --build build --parallel
ctest --test-dir build
```

### Installation

- Build output: `build/plugins/adapters/phi_adapter_hue_ipc`
- Installed by the Debian package to `/usr/lib/<multiarch>/phi/plugins/adapters/`

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

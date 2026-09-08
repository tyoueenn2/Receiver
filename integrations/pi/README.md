# Raspberry Pi telemetry integration

The receiver is a separate application from [usb-proxy-udp](https://github.com/tyoueenn2/usb-proxy-udp). This directory contains an optional extension patch for that project; it is required for the receiver's physical-button activation.

`telemetry.patch` is based on proxy commit `c3c09153feabe4be2eb3a711fe3c3e957f6eb794`. It adds UPS1/UPT1 subscriptions and tests while preserving UPX1 commands. The proxy's restoration commit `562ce96` has identical files and also accepts this patch.

In a clean clone of the proxy repository, apply the patch using its absolute location in this Receiver checkout:

```sh
git apply --check /path/to/Receiver/integrations/pi/telemetry.patch
git apply /path/to/Receiver/integrations/pi/telemetry.patch
make test
make
```

Use the proxy's existing setup instructions and enable `--enable_injection`. See [the wire protocol](../../docs/PROTOCOL.md) and [validation limits](../../docs/VALIDATION.md). The patch has been checked for clean application locally. Linux integration tests and physical USB operation still require validation on the Pi.

The patch includes the proxy usage updates, telemetry codec, and tests. Its upstream Apache-2.0 license is provided in `LICENSE`.

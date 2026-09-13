# Raspberry Pi/proxy compatibility

Receiver and the proxy are separate projects. The public proxy repository is authoritative for Pi, Raw Gadget, USB-writer, scheduler, and watchdog behavior. Do not apply or generate a Receiver-side source patch as a substitute for the current proxy implementation.

Full Receiver functionality requires UPX1, UPS3/UPT3, and UPC1/UPA1 version 2. UPT1 and both 80-byte and legacy 88-byte UPT2 remain supported at the reduced capability described in [the wire protocol](../../docs/PROTOCOL.md). Receiver never silently sends version-1 click timing because its interval semantics differ.

Build, test, and run the proxy according to that project's current instructions, with injection enabled. Then configure Receiver to use the Pi's IPv4 address and command port. All Receiver traffic uses one persistent UDP source port so proxy controller ownership remains stable.

Receiver's `tools/mock_pi.py` is a deterministic test fixture only. It has no Raw Gadget or USB access and cannot validate a real Pi, physical mouse, USB host, or destination application.

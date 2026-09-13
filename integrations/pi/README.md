# Raspberry Pi/proxy compatibility

Receiver and the proxy are separate projects. The public proxy repository is authoritative for Pi, Raw Gadget, USB-writer, scheduler, and watchdog behavior. Do not apply or generate a Receiver-side source patch as a substitute for the current proxy implementation.

Full Receiver functionality requires UPX1, UPS3/UPT3, and UPC1/UPA1 version 2. UPT1 is the negotiated reduced-capability fallback. Canonical 80-byte and legacy 88-byte UPT2 packets remain decoder-tested, but Receiver does not request UPT2: proxy revision `ef4abe246a614bd22cc21116fd14ecc885edbb60` uses a conflicting 80-byte layout without a discriminator. See [the wire protocol](../../docs/PROTOCOL.md). Receiver never silently sends version-1 click timing because its interval semantics differ.

`tests/proxy_codec_vectors.cpp` contains checked-in golden UPX1, UPC1 v2, UPA1 v2, UPT3, canonical UPT2, and incompatible audited-proxy UPT2 packets derived from that pinned proxy revision. It proves the compatible layouts decode exactly and the known incompatible UPT2 vector is rejected without guessing.

Build, test, and run the proxy according to that project's current instructions, with injection enabled. Then configure Receiver to use the Pi's IPv4 address and command port. All Receiver traffic uses one persistent UDP source port so proxy controller ownership remains stable.

Receiver's `tools/mock_pi.py` is a deterministic test fixture only. It has no Raw Gadget or USB access and cannot validate a real Pi, physical mouse, USB host, or destination application.

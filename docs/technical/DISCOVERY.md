# LAN discovery

## Receiver finds a Raspberry Pi

End the session and choose **Connections > Find Raspberry Pi**. Receiver searches
for about two seconds. A single match is selected automatically. Multiple
matches appear as **Use Pi ADDRESS:PORT** buttons. Selection switches to connected
devices with Pi mouse output and saves `pi_ip` and `pi_port` to
`user-settings/setup.json`. Other setup-file edits are preserved. Start the
session to connect; discovery does not arm assistance.

The Pi must run a USB Proxy UDP build supporting `UPQ1` / `UPI1` capability
queries, including the current proxy in this workspace. Discovery can find a Pi
before its USB mouse endpoint is ready. These read-only queries neither send
movement nor take over the active controller's telemetry subscription.

The search broadcasts to active IPv4 interfaces and checks loopback, using
`pi_port` from setup (default 12345). Set a custom proxy port in the file and
reload it before searching. If `USB_PROXY_PEER` is set on the Pi, it must allow
the Receiver PC's address. Firewalls, Wi-Fi client isolation and routed networks
can prevent discovery. Manual `pi_ip` configuration remains available.

## FrameSender finds Receiver

Open an updated Receiver with **UDP sender** selected as its picture source.
Its session may be stopped. In FrameSender, click **Find Receiver**. One match
fills the Receiver address and video port; several matches appear in the address
dropdown. Then start the Receiver session and click **Start sending**.

Receiver advertises its configured video port, including custom ports. The
Receiver PC needs inbound UDP **39570** for discovery and its configured video
port for frames. Keep `sender_ip` in Receiver's setup file configured for the
sending PC; discovery does not change the accepted video sender.

Normal GUI launches advertise before a session starts; live headless UDP
sessions also advertise. Demo and smoke-test launches do not advertise. A local
desktop capture source does not advertise a video receiver. One Receiver
discovery service can bind port 39570 per PC. If the port is unavailable, the
app reports that discovery is unavailable and manual connections still work.

## Receiver discovery wire format

The portable implementation is mirrored in
`Receiver/include/receiver/lan_discovery.hpp` and `Sender/src/lan_discovery.hpp`
so the repositories can build independently. All integers use network byte order.

Queries go to UDP 39570. Replies go to the querying source address and ephemeral
port. Requests are retried every 300 ms for 1600 ms. Results are deduplicated by
address and advertised service port and capped at 32. Searches and responders
run away from the GUI thread.

| Offset | Query, 16 bytes | Reply, 24 bytes |
| --- | --- | --- |
| 0 | `UVDQ` | `UVDA` |
| 4 | Version 1, one byte | Version 1, one byte |
| 5 | Zero, one byte | Zero, one byte |
| 6 | Zero, two bytes | Video UDP port, two bytes, nonzero |
| 8 | Random nonzero nonce, eight bytes | Echoed query nonce, eight bytes |
| 16 | — | Bound IPv4 address, four bytes; zero means use the reply source address |
| 20 | — | Zero, four bytes |

Clients reject malformed, oversized, reserved-field, wrong-version and wrong-nonce
replies. Receiver replies are rate limited. Discovery identifies LAN endpoints;
it does not authenticate them or alter video ownership, mouse state or arming.

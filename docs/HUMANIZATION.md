# Humanization controls

Open **Humanization** to adjust sensitivity, smoothing, the dead zone, sticky targeting, movement style, random variation, and direction-based strength. These controls are saved with your settings. Existing profiles load with the new effects off, preserving their previous movement behavior.

## Direction-based help

Enable **Adjust help to my mouse direction** and choose a behavior:

- **More help when moving away:** reduces help when you already move toward the aim point and increases help when you move away.
- **More help when moving toward:** reverses that relationship.
- **Different help for left and right:** applies your left/right multipliers when moving toward a target on that side. Moving away keeps normal strength. Diagonal targets interpolate between the side values.

**Strength of this effect** blends the directional difference from none to full. In the first two modes, a 50% setting gives 50% of normal help in one direction and 150% in the other. Sideways movement gives 100%. This is a multiplier on the configured sensitivity, subject to the existing movement cap.

**Help when barely moving** is an independent multiplier. **Slow movement threshold** controls the transition between that multiplier and the direction-based multiplier. Reduce the threshold if a small, slow hand movement should count as intentional direction. **Direction averaging** smooths the measured physical velocity over time; larger values respond more slowly.

The implementation compares the angle between physical mouse velocity and the vector from the configured picture reference to the selected aim point. With cosine `c`, effect strength `k`, physical speed `v`, threshold `v0`, and barely-moving multiplier `b`:

```
moving = 1 - k*c              # more help away
moving = 1 + k*c              # more help toward
blend = clamp(v/v0, 0, 1)
multiplier = b + blend*(moving-b)
```

The left/right mode uses `moving = 1 + k*max(c,0)*(side-1)`, where `side` blends the configured left and right multipliers using the target's horizontal direction. All multipliers remain between 0% and 200%. Angles assume positive physical X moves right and positive Y moves down in the target view; inverted/custom axis mappings require calibration outside this initial implementation. Counts/second depend on mouse DPI, so tune the slow threshold for your mouse.

With the Pi connection, physical mouse data comes from the Pi's **UPS2/UPT2 extension**, never from corrections sent by Receiver. A first snapshot establishes the counter baseline; a second gives velocity. Missing, stale, discontinuous, or unavailable motion data pauses direction-enabled assistance. An old button-only Pi can still be used with direction-based help off. See [Pi installation](../integrations/pi/README.md).

## Movement styles

**Enable movement shaping** controls the style and random-variation effects. Sensitivity, smoothing, and direction-based help remain independent.

| Style | Behavior |
| --- | --- |
| Direct | Uses the normal straight correction. |
| Curved | Adds a cubic Bezier perpendicular bend that fades over the selected build-up time. |
| Ease in | Builds from zero to normal strength over the selected time using an adjustable exponent. |
| Adaptive | Reduces strength near the aim point, reaching full strength at the chosen approach distance. |
| Smooth noise | Adds smoothly interpolated random variation perpendicular to the correction. |

**Small random variations** adds bounded jitter in mouse units at a configurable interval. Both jitter and smooth noise fade within 20 picture pixels of the aim point and are disabled inside the dead zone. No variation occurs without a current eligible target. Smoothing is time-based exponential averaging. Shaping state resets on target change, deactivation, stale input, and configuration changes; no future path points are queued. Direction strength is applied after shaping/smoothing and before the existing device/per-update caps. Zero strength clears fractional and smoothed leftovers.

Aimmy's [movement settings](https://github.com/Babyhamsta/Aimmy/blob/Aimmy-V2/Aimmy2/Class/Dictionary.cs) and [movement implementation](https://github.com/Babyhamsta/Aimmy/blob/Aimmy-V2/Aimmy2/InputLogic/MouseManager.cs) informed the selection of sensitivity, jitter, smoothing, sticky targeting, and movement-style controls. This is an independent implementation; its numerical behavior is not a port of Aimmy. Smooth noise uses interpolated random values rather than a Perlin routine. Automatic clicking is not included.

## Practice and checks

The demo uses steady stationary simulated physical input by default. Start the launcher with `--cycle-motion` to cycle right, left, slowly right, then stationary every four seconds. Enable direction-based help and **Enable practice movement** to see the last calculated multiplier. These are simulated mouse readings, not your local desktop cursor. Your actual mouse is never moved by the simulator.

Run the CMake tests and `python tests/direction_integration_test.py path/to/receiver_headless.exe` to check both angle modes, the slow-input fallback, missing/stale telemetry, a motion-generation restart, and activation release. Pi USB operation and deployment-GPU latency still require hardware validation.

## Additional Aimmy-inspired controls

These controls follow the configuration concepts in Aimmy's [settings](https://github.com/Babyhamsta/Aimmy/blob/Aimmy-V2/Aimmy2/Class/Dictionary.cs) and [prediction manager](https://github.com/Babyhamsta/Aimmy/blob/Aimmy-V2/Aimmy2/AILogic/PredictionManager.cs). They are independent implementations, not numerically identical ports.

- **Lead moving targets:** choose Kalman filtering, exponentially averaged velocity, or a five-sample velocity history. Lead time and multiplier adjust how far ahead to aim; maximum lead distance limits displacement. Velocity averaging applies to the averaged-velocity method. Capture timestamps determine elapsed time. A missing detection, changed identity, deactivation, stale frame, or sample gap over 100 ms resets prediction. Predicted points stay within the picture and active search circle.
- **Sticky target distance:** lets persistence match a current detection of the same class within a center-distance threshold, even when boxes no longer overlap. Zero uses overlap only. This is geometric matching, not a learned identity tracker.
- **Dynamic search area:** holding a selected physical button substitutes another search radius. Releasing it restores the usual radius. It does not activate movement by itself.
- **EMA smoothing:** replaces time-based movement smoothing with an adjustable per-update response. Smaller values respond more gradually. Unlike time-based smoothing, its feel depends on update frequency.
- **Alternate activation:** the Mouse tab can enable a second hold-to-activate button. Either button works; enabling movement at the top is still required.

## Local Windows mouse testing

1. Open the demo and press **Stop session**.
2. Open **Mouse** and choose **Local Windows mouse** under Mouse connection.
3. Start practice. The example image uses a fixed detection, so it does not track desktop content.
4. Enable **local cursor movement**, then press and hold your activation button (right mouse by default).
5. Release it, press Delete, or select **Turn movement off** to stop.

This connection reads physical Windows mouse events and uses `SendInput` for relative cursor movement. Injected motion and buttons are excluded from the physical readings. It supports the five standard mouse buttons. Press the activation button after starting; a button already held at startup is not trusted.

Local direction readings use cursor position differences, not raw HID counts. Windows pointer acceleration and screen boundaries affect them, so local slow-speed settings are not directly transferable to the Pi. Windows can block output into higher-privilege applications. No synthetic clicks are sent. The same fresh-frame and activation gates apply; changing connections requires stopping the session.

The automatic local-mouse check only starts the reader and verifies state. Actual cursor output and feel require the explicit manual steps above. See [Test Hub](TEST_HUB.md).

The practice image shows a white reference cross and a bullseye centered on the simulated detection. The visible movement status and correction counter help distinguish paused output from a desktop cursor at a screen boundary. Local control uses a high-resolution waitable timer that also wakes for mouse messages; one correction is still computed per fresh result. Expired frames are never replayed to manufacture continuous movement.

The production Windows control path now wakes on new results and mouse/network events, superseding the timer-polled path described in the earlier validation entry. Optional motion smoothing and easing still intentionally change response. See [latency measurements](BENCHMARK.md).

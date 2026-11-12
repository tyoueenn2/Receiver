# HSV color targeting

In **Targeting → Target source**, choose one of these modes:

| Mode | Behavior |
| --- | --- |
| Model | Existing model, class preference and box aim settings. |
| HSV color only | Select the topmost matching pixel in the search area, without loading a model, Python or CUDA. |
| Model + HSV color | Select a model box using the normal confidence, class, priority and persistence rules. Refine its aim to the highest matching pixel inside that box. If no eligible pixel matches, use the normal box aim point. |

Both color modes work with UDP RGB/BGRA frames and local Windows Desktop
Duplication capture. No injection into the captured application is involved.
Switching target source while running disarms assistance; arm again when ready.
HSV bounds and offsets can be adjusted live.

## Range and offsets

- Hue is **0–360 degrees**, saturation and value are **0–100 percent**. Bounds are inclusive. These are not OpenCV's 0–179 hue units.
- A minimum hue above the maximum wraps through red. The default range of **330–30**, saturation **60–100**, value **50–100** detects bright saturated reds.
- Saturation and value minima must not exceed their maxima. Alpha is ignored.
- Highest means the smallest image Y coordinate. Pixels on the same row are ranked by horizontal distance from the crosshair; exact ties choose the left pixel. Coordinates use pixel centers.
- Color offsets are image pixels: positive X moves right, positive Y moves down. They replace the normal box offsets for a color match. The ordinary aim fraction and offsets still apply to the hybrid fallback.
- Offsets are clamped to the frame in color-only mode, or to the selected box in hybrid mode. The resulting aim must be inside the current search radius, including a button-controlled radius when enabled.
- An absent color produces no target in color-only mode. Hybrid mode never uses pixels outside the selected box and never changes to another box just because it contains color.

Use **Preview → Enable preview window** and **Show color aim in preview** to tune the
range while disarmed. The preview displays the matching-pixel count and a yellow
cross for the selected aim, including the hybrid fallback. Actual control can
also apply prediction and movement settings. Model confidence and class controls
are unused in color-only mode. **Capture size** controls the local crop and the
size requested from FrameSender. Manual UDP senders keep their chosen dimensions.

Color settings are saved in profiles and application preferences. Selecting a
different model keeps the current color mode and range. Existing profiles default
to Model mode. Model benchmarking is unavailable in color-only mode.

## Color-only automatic clicks

In **Auto click**, enable automatic clicks. With **HSV color only**, a click
requires matching pixels in all three directions: **left, right, and above**
the crosshair. The center pixel and pixels below the crosshair are not required.
This uses the original color mask, independently of aim offsets and prediction.

- **Color scan distance** limits each direction to 1–1024 image pixels (default 32).
- **Color scan tolerance** widens each scan on either side of its axis (default 2).
  Left/right scan horizontal strips; above scans a vertical strip. Set tolerance
  to 0 for an exact row/column check. Distance and tolerance boundaries are inclusive.
- Scans use the pixel containing the crosshair, defaulting to screen center, or
  the custom Targeting crosshair reference. Scans stop at the image edge; the
  crosshair's own pixel cannot satisfy any direction.
- An eligible color aim inside the search radius is still required. Matches in
  the three strips need not belong to the same connected outline.

All three directions must remain present for **Wait before first click**. In
single-click mode, continuous occupancy fires once even if the topmost aim pixel
changes. A missing direction resets dwell and allows a new single click on
reacquisition. Repeat mode uses the existing click interval and hold duration.
Changing scan distance or tolerance disarms assistance. Activation, fresh-frame,
telemetry, physical-button and pending-click gates still apply. Automatic clicks
continue to require Raspberry Pi output with scheduled-click support.

**Model** and **Model + HSV color** retain the existing selected-model-box click
condition; these scan settings apply only to **HSV color only**.

## Reducing color false positives

HSV thresholds are a useful starting point for colored outlines because hue can
be tuned independently of brightness. Tighten saturation/value bounds and keep
the search radius and trigger distance small enough to exclude unrelated UI.
Use the click delay to reject brief matches.

For a more robust color-only detector, connected components with minimum size
and plausible dimensions can reject isolated noise and keep the three trigger
directions on one outline. A short continuity check across frames can further
reduce flicker. These are possible extensions, not current filtering options.
The absolute topmost pixel can still jump to a stray match; the midpoint of a
component's top band would be a steadier alternative if exact topmost targeting
is not needed. When inference is acceptable, the existing hybrid mode narrows
color aiming to a selected model box.

## Headless profile

Set `color.mode` to `0` (model), `1` (color only), or `2` (model + color):

```json
{
  "version": 1,
  "color": {
    "mode": 1,
    "min_h": 330, "max_h": 30,
    "min_s": 60, "max_s": 100,
    "min_v": 50, "max_v": 100,
    "offset_x": 0, "offset_y": 4,
    "preview": true
  },
  "trigger": {
    "enabled": true,
    "color_radius": 32,
    "color_tolerance": 2,
    "delay_ms": 100
  }
}
```

Run `receiver_headless --profile colors.json --seconds 0`. You can override the
source with `--target-mode model|color|model-color`. Normal activation, telemetry,
frame freshness and disarming rules apply in every mode.

HSV conversion scans each frame once on the image worker. A pooled bit mask is
shared read-only with control and preview, so RGB conversion does not block the
mouse output thread. The automated suite covers RGB/BGRA, hue wrapping, clipping,
offsets, dynamic radius, directional triggering, profile compatibility and real
App/UDP mode transitions.

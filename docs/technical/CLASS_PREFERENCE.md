# Class preference

Open **Targeting > Class preference**, select **Preferred class**, and choose a
**Preference type**. **Off** retains the existing targeting behavior.

## Preference types

**Whenever available** selects an eligible detection of the preferred class
before considering normal targets. For example, prefer a head anywhere in the
search area, falling back to a player when no eligible head is visible. The
closest-to-crosshair or highest-confidence priority and current-target lock apply
within the preferred class. A new preferred detection can replace a locked normal
target.

**Near the normal target** first selects the normal target using its class filter,
priority, search radius and current-target lock. It then selects a preferred
detection close to that specific target. A head near another player does not make
Receiver switch players to find a head. Without a qualifying preferred detection,
the original target remains selected; without a normal target, nothing is selected.
The preferred class cannot serve as its own normal target in this mode.

For the head/player example:

1. Set **Target class** to `player`.
2. Set **Preferred class** to `head`.
3. Set **Preference type** to **Near the normal target**.
4. Adjust **Maximum association distance**, initially 32 image pixels.

Distance is measured from the preferred box's center to the nearest point on the
normal target's rectangle. A center inside the player's box has distance zero;
a head just above it can qualify within the configured margin. This uses a
Euclidean distance, including at corners. Pixels refer to the captured/received
image, not the preview's on-screen display size. If several preferred boxes
qualify, the current preferred target is retained when target lock matches;
otherwise the one whose center is nearest the normal target's aim point wins.
The normal target lock persists through body-to-head and head-to-body transitions.

## Filters and saved settings

The preferred class is admitted in addition to **Target class**; selecting only
`player` does not discard `head` predictions before preference can run. Both
normal and preferred candidates still need the minimum confidence and an aim
point inside the current search radius, including the alternate held radius.
Same-class duplicate-box suppression remains in effect.

Preferences are saved per model and in JSON profiles. Class names remap preferred
IDs when model labels are reordered. An unavailable class is disabled rather than
reassigned to an unrelated class. A model with no saved settings starts with
preference off. When names are unavailable, **Preferred class ID** accepts a model
class number; `-1` disables it.

The JSON fields are:

```json
"class_preference": {
  "class_id": 1,
  "class_name": "head",
  "mode": 1,
  "max_distance": 32
}
```

`mode` is `0` for whenever available and `1` for near the normal target. Old
profiles default to preference off. Proximity is a geometric heuristic, not proof
that two detections belong to the same object; overlapping players and noisy
boxes can still be ambiguous.

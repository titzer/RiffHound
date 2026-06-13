# How to Draw a Piano Roll Correctly

Hard-won lessons from iterating on a canvas-rendered piano keyboard with an
aligned frequency history plot.

---

## The Two Coordinate Systems

A piano has **two different x-axis grids** that must coexist:

| Section | Name | Formula | Description |
|---------|------|---------|-------------|
| Top (black key region) | Chromatic | `u = w / N` | Every semitone gets the same slot width. N = total semitones in range. |
| Bottom (white key bodies) | White-key | `wkW = u * 12 / 7` | Every white key gets the same body width. |
| Frequency history plot | Chromatic | `u = w / N` | Always pure chromatic — equal semitone widths. |

These grids are different. White keys are wider than one chromatic unit (`wkW ≈ 1.71u`), so white key bodies extend under the black keys. This is what gives keys their L-shape and backwards-L-shape.

---

## The White Key Body Width Formula

**Use `wkW = u * 12 / 7`, not `w / numWhiteKeys`.**

With `wkW = 12u/7`, seven white keys span exactly one chromatic octave (`7 × 12u/7 = 12u`).
This makes every B-C (octave) boundary in the bottom section fall exactly on a chromatic
position — with zero extra math, zero accumulated error across multiple octaves.

Using `w / numWhiteKeys` instead creates a slightly different `wkW`. The bottom dividers
drift away from the chromatic positions by an amount that grows with each octave. At 3
octaves the rightmost B-C divider can be off by ~15px on a typical screen. Very noticeable.

**Caveat**: this formula assumes the range starts and ends on C (i.e., `maxMidi - minMidi`
is a multiple of 12). That is almost always the case for a piano roll.

---

## Rendering Passes (draw in this order)

```
Pass 0: background fill
Pass 1: white key BOTTOM bands  (y = bkH → h, white-key grid)
Pass 2: dividers between white key bottoms
Pass 3: white key TOP stubs     (y = 0 → bkH, chromatic grid)
Pass 4: dividers between adjacent top stubs (E-F and B-C only)
Pass 5: black keys              (y = 0 → bkH, centered in chromatic slot)
Pass 6: labels
```

The top section height: `bkH = Math.round(h * 0.62)` is a good proportion.
Black key width: `bkW = Math.round(u * 0.65)`.

---

## The E-F and B-C Divider Alignment Problem

**This is the trickiest part.** In the top section, dividers between adjacent white keys
(E-F and B-C) are drawn at chromatic positions. In the bottom section, the dividers are
at white-key-grid positions. These only align if you handle them explicitly.

With `wkW = 12u/7`:
- **B-C boundaries align automatically** because `7 × wkW = 12u` exactly.
- **E-F boundaries do NOT align automatically** because E is the 3rd white key in an
  octave, and `3 × wkW = 36u/7 ≈ 5.14u`, not `5u` (the chromatic position of F).

### The Fix: anchor E and F to the chromatic F position

```javascript
function wkLeftX(m) {
    var pc = ((m % 12) + 12) % 12;
    if (pc === 5) return Math.round((m - minMidi) * u); // F: chromatic anchor
    return Math.round(wkIdx[m] * wkW);
}

function wkRightX(m) {
    var pc = ((m % 12) + 12) % 12;
    if (pc === 4) return Math.round((m + 1 - minMidi) * u); // E: right = chromatic F
    if (wkIdx[m] + 1 >= numWhite) return w;
    return Math.round((wkIdx[m] + 1) * wkW);
}
```

Use `wkLeftX(m)` and `wkRightX(m)` for **all** bottom-section drawing: key bands,
dividers, label centering, and hit testing.

The invariant `wkRightX(m) === wkLeftX(nextWhiteKey(m))` holds for all keys, so
bands and dividers are always pixel-consistent with each other.

Side effect: E is very slightly narrower than other white keys (`11u/7` vs `12u/7`),
and F is slightly wider (`13u/7`). The difference is ~3px on a typical screen and
matches how real piano keys actually look.

---

## The Frequency History Plot

The history plot maps frequency → x using **pure chromatic coordinates only**.
Do not mirror the bottom white-key grid into the history plot.

```javascript
// x-position for a given frequency
function fx(freq) {
    if (freq <= 0) return -10;
    var midiVal = freqToMidi(freq); // returns fractional MIDI number
    midiVal = Math.max(minMidi, Math.min(maxMidi, midiVal));
    return (midiVal - minMidi + 0.5) * u; // center of chromatic slot
}
```

This places every semitone in an equal-width column, which is correct for a
pitch display. It aligns with the top section of the piano (equal-width stubs),
not with the bottom section (wider white key bodies). That is intentional.

For background shading of black key columns in the history plot:
```javascript
// Shade the full chromatic slot for each black key
xLeft  = Math.round((m - minMidi) * u);
xRight = Math.round((m - minMidi + 1) * u);
```

For white key separator lines in the history plot:
```javascript
// Draw at the left edge of each white key's chromatic slot
gx = Math.round((m - minMidi) * u);
```

---

## Hit Testing

Hit test in two separate regions:

**Top section (`clickY < bkH`)**: test black keys first (they sit on top), then fall
back to whichever chromatic slot the click landed in.

```javascript
// Black keys
for each black key m:
    xc = (m - minMidi + 0.5) * u;
    kx = Math.round(xc - bkW / 2);
    if (clickX >= kx && clickX < kx + bkW) return m;

// White key stubs
slot = Math.floor(clickX / u);
slotMidi = minMidi + slot;
if (!isBlack(slotMidi)) return slotMidi;
```

**Bottom section (`clickY >= bkH`)**: iterate white keys and check the
`[wkLeftX(m), wkRightX(m))` interval (using the same anchored formula as drawing).

---

## Key Facts About the Black Key Pattern

```
Pitch classes that are black keys: [1, 3, 6, 8, 10]
function isBlack(m) { return [1,3,6,8,10].indexOf(((m%12)+12)%12) >= 0; }
```

Adjacent white-white pairs (no black key between them): **E-F** (pc 4-5) and **B-C** (pc 11-0).
These are the only pairs that need a visible divider in the top stub section.

---

## Summary Checklist

- [ ] `u = w / N` for chromatic grid (top stubs + history plot)
- [ ] `wkW = u * 12 / 7` for white key body width (not `w / numWhiteKeys`)
- [ ] Range starts and ends on C (so `N mod 12 == 1` for inclusive ranges like C2–C5)
- [ ] `wkLeftX` anchors F to chromatic position; `wkRightX` anchors E to chromatic F
- [ ] Draw passes in order: white bottoms → bottom dividers → white tops → E-F/B-C top dividers → black keys → labels
- [ ] History plot uses pure chromatic `u` throughout — never mixes in `wkW`
- [ ] Hit test uses same `wkLeftX`/`wkRightX` as drawing (not a separate formula)
- [ ] `wkRightX(m) === wkLeftX(nextWhite(m))` for all m — verify this invariant holds

# Converter exactness classes

A conversion report carries two independent fields that never share a label space: one operation label from the table below, and one metadata outcome (`preserved`, `stripped-explicitly`, or `unsupported-items-named`). A result may only be reported with the label it actually proved.

| Operation label | What is proved | What it does not promise |
| ----- | -------------- | ------------------------ |
| `pixel-exact` | A fresh decode of the produced file yields the same dimensions, sample format, native bit depth, alpha mode (including RGB under transparent pixels), every native sample, frame count and frame durations as the agreed source. | Identical file bytes, metadata preservation, or identical encoded coefficients. |
| `byte-identical` | The produced file is byte-for-byte the source file (a copy), verified by hashing both files. | That a decode/re-encode would produce the same bytes, or that any pixel comparison ran. |
| `coefficient-exact` | For JPEG optimization and import: the quantized DCT coefficients and JPEG coding parameters are preserved exactly, checked in the coefficient domain. | Identical decoded pixels from unrelated IDCT implementations, or byte-identical output. |
| `original-byte-reconstruction` | Decoding a JPEG XL file produced from JPEG reconstructs the original JPEG bytes exactly, verified by hashing the reconstruction against the source JPEG. This is its own label: it is not `byte-identical`, which names a copy. | A pixel comparison beyond what the reconstruction implies. |

The metadata outcome is not an operation label: `preserved` means every metadata item the target format can carry is present with equal meaning (ICC bytes, orientation, frame timing), checked item by item; items the target cannot represent are named in the report, never dropped silently. `metadata=strip` conversions report `stripped-explicitly` and can never claim byte-oriented labels whose bytes the strip changes.

Rules:

- No preset, setting or default may reduce sample precision, flatten or premultiply alpha, discard frames, or drop metadata silently. The registry refuses combinations that would (`frames=first-only` for an exact conversion; `metadata=strip` with `exactness=byte-identical`).
- A larger lossless result is reported as growth, never hidden.
- Preview or color-managed pixels are never proof of any class. Verification decodes the produced file afresh with the bundled decoder and compares native samples (owned by D10 T01 §4).
- A class the target format cannot support stays visible in the capability registry with its reason, instead of being approximated under the same name.

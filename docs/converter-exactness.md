# Converter exactness classes

A conversion carries exactly one exactness class. The classes never share a label, and a result may only be reported with the class it actually proved.

| Class | What is proved | What it does not promise |
| ----- | -------------- | ------------------------ |
| `pixel-exact` | A fresh decode of the produced file yields the same dimensions, sample format, alpha mode (including RGB under transparent pixels), every native sample, frame count and frame durations as the agreed source, under the named metadata policy. | Identical file bytes, identical metadata bytes, or identical encoded coefficients. |
| `byte-identical` | The produced file is byte-for-byte the source file. Only a copy or a format-native reconstruction path (for example JPEG reconstruction through JPEG XL) may claim it, and only after hashing both files. | That a re-encode would produce the same bytes. |
| `metadata-preserved` | A modifier on `pixel-exact`: every metadata item the target format can carry is present with equal meaning (ICC bytes, orientation, frame timing), checked item by item. `metadata=strip` conversions can never carry this modifier. | That metadata the target cannot represent survived: unsupported items are a distinct, named outcome, never a silent drop. |
| `coefficient-exact` | For JPEG optimization and import: the quantized DCT coefficients and JPEG coding parameters are preserved exactly, checked in the coefficient domain. | Identical decoded pixels from unrelated IDCT implementations. |
| `original-byte-reconstruction` | A JPEG XL file produced from JPEG reconstructs the original JPEG bytes exactly (the `byte-identical` claim for that pair), verified by hashing the reconstruction against the source. | Pixel equality beyond what the reconstruction implies. |

Rules:

- No preset, setting or default may reduce sample precision, flatten or premultiply alpha, discard frames, or drop metadata silently. The registry refuses combinations that would (`frames=first-only` for an exact conversion; `metadata=strip` with `exactness=byte-identical`).
- A larger lossless result is reported as growth, never hidden.
- Preview or color-managed pixels are never proof of any class. Verification decodes the produced file afresh with the bundled decoder and compares native samples (owned by D10 T01 §4).
- A class the target format cannot support stays visible in the capability registry with its reason, instead of being approximated under the same name.

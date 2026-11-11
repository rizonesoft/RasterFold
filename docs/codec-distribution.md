# Codec distribution decisions

This page records the release owner's decisions about codecs whose distribution carries licence or patent obligations. It is owned by `D99 T01 §7`; `D10 T02 §7` implements what it decides.

## HEIF and HEIC (HEVC)

**Decision (operator, 2026-11-11): "OS codec, read only".** The converter bundles no HEVC decoder and no HEVC encoder. It imports HEIF/HEIC images only through the Windows system codec (the Windows Imaging Component HEIF decoder) when the user's system provides it, and it never writes HEIC.

Why: HEIC images are compressed with HEVC, which is patent-licensed. Distributing an HEVC decoder or encoder inside the application can create royalty obligations to the HEVC patent pools, and the usual open-source encoder, x265, is GPL, which would put the converter under the GPL or require a commercial licence. Using the codec the operating system already carries moves that obligation off this project. AVIF, which is royalty free, covers the modern-format case for writing.

What it costs: HEIC import depends on the user's system. Where the system codec is absent the converter refuses the file with a reason that names the missing system component; it does not fall back to a bundled decoder.

Reversal cost: bundling a decoder later (for example libde265 under the LGPL) needs a legal review of the patent position before any public release, a pinned source in `converter/vendor.json`, and a new decision recorded here.

Still owed by `D99 T01 §7` before it is stamped: the exact Windows component names and the Windows versions that carry them, confirmed from Microsoft's documentation, and the intended distribution channels.

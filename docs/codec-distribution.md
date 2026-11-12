# Codec distribution decisions

This page records the release owner's decisions about codecs whose distribution carries licence or patent obligations. It is owned by `D99 T01 §7`; `D10 T02 §7` implements what it decides.

## HEIF and HEIC (HEVC)

**Decision (operator, 2026-11-11): "OS codec, read only".** The converter bundles no HEVC decoder and no HEVC encoder. It imports HEIF/HEIC images only through the Windows system codec (the Windows Imaging Component HEIF decoder) when the user's system provides it, and it never writes HEIC.

Why: HEIC images are compressed with HEVC, which is patent-licensed. Distributing an HEVC decoder or encoder inside the application can create royalty obligations to the HEVC patent pools, and the usual open-source encoder, x265, is GPL, which would put the converter under the GPL or require a commercial licence. Using the codec the operating system already carries moves that obligation off this project. AVIF, which is royalty free, covers the modern-format case for writing.

What it costs: HEIC import depends on the user's system. Where the system codec is absent the converter refuses the file with a reason that names the missing system component; it does not fall back to a bundled decoder.

Reversal cost: bundling a decoder later (for example libde265 under the LGPL) needs a legal review of the patent position before any public release, a pinned source in `converter/vendor.json`, and a new decision recorded here.

### The provider, by name

The converter ships no HEVC code on any channel: no decoder, no encoder, and no HEVC library linked or bundled. HEIC import goes through these Windows components, all from Microsoft (read 2026-11-12):

| Component | What it does | Identifier | Where it comes from | Source |
| --------- | ------------ | ---------- | ------------------- | ------ |
| WIC HEIF decoder | Reads the HEIF container and hands its HEVC-coded image to the video decoder | `CLSID_WICHeifDecoder` for the container `GUID_ContainerFormatHeif` | The HEIF Image Extensions package, Microsoft Store product `9PMMSR1CGPWG` | [HEIF extension codec](https://learn.microsoft.com/windows/win32/wic/heif-codec), [WIC GUIDs and CLSIDs](https://learn.microsoft.com/windows/win32/wic/-wic-guids-clsids) |
| HEVC video decoder | Decodes the HEVC bitstream inside a `.heic` file | The Media Foundation H.265 decoder, found with `MFTEnumEx` for `MFVideoFormat_HEVC` (`hevcdecoder.dll`, `hevcdecoder_store.dll`) | The HEVC Video Extensions package, Microsoft Store product `9N4WGH0Z6VHQ`, unless the device already provides it | [H.265 / HEVC Video Decoder](https://learn.microsoft.com/windows/win32/medfound/h-265---hevc-video-decoder), [Troubleshoot video playback in Microsoft Edge](https://learn.microsoft.com/troubleshoot/microsoft-edge/development/video-playback-issues) |

Windows versions: for the HEIF side, Microsoft documents Windows 10, version 1809 as the minimum client for the WIC HEIF interfaces ([WICHeifProperties enumeration](https://learn.microsoft.com/windows/win32/api/wincodec/ne-wincodec-wicheifproperties)); Microsoft Learn states no separate minimum for the HEIF Image Extensions package itself, so the converter treats 1809 as the earliest Windows on which HEIC import can work. For the HEVC side, the H.265 decoder's documented minimum client is Windows 10 (desktop apps), and the HEVC Video Extensions package needs Windows 10 version 16299.0 or later ([Windows in education](https://learn.microsoft.com/microsoft-365/education/guide/1-windows/windows-products-edu)). Microsoft states that HEVC support is available starting with Windows 11, version 22H2, with hardware decoding where the device has it and software decoding otherwise ([What's new in Windows 11, version 22H2](https://learn.microsoft.com/windows/whats-new/whats-new-windows-11-version-22h2)). Both components are therefore needed on Windows 10, version 1809 or later. Microsoft's HEIF page still says the HEVC codec "might not be available on all PCs". The converter therefore never infers support from the Windows version: it asks WIC for the HEIF decoder and Media Foundation for an HEVC decoder each time it opens a HEIC file.

### Distribution channel

The converter is distributed on the repository's own [GitHub Releases](https://github.com/rizonesoft/RasterFold/releases) page, the channel every RasterFold artifact uses. No channel carries HEVC code, so the channel creates no HEVC obligation. Adding a channel later (a package manager or a store) costs one edit to this page.

### Without the system components

A user whose Windows lacks either component can still use the converter for every other format. Opening a `.heic` or `.heif` file is refused, and nothing is written. The refusal names what is missing and where to get it: the HEIF Image Extensions or the HEVC Video Extensions from the Microsoft Store. HEIC is never offered as an output format on any system.

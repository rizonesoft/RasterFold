# Writes the two RasterFold campaign shortcuts on this Windows user's Desktop (docs/unattended-run.md, The Desktop
# shortcuts) and removes the old "RasterFold Unattended Run" one:
#   "RasterFold Claude campaign"  wsl.exe ... bash scripts/unattended-run.sh         (Claude Code, latest Opus, medium)
#   "RasterFold Carbon campaign"  wsl.exe ... bash scripts/unattended-run.sh carbon  (Antigravity, Orchestrator (Carbon))
# Run it on Windows from the WSL checkout, for example:
#   pwsh -File \\wsl.localhost\Ubuntu\home\<user>\projects\RasterFold\scripts\install-launchers.ps1
# The distro and the Linux path of the checkout are read from where this file is, so the shortcuts follow the clone;
# run it again after moving the clone.
#
# Each icon is the RasterFold mark (assets/brand/icon.svg) with its runner's badge in the corner, as the Isotone campaign
# shortcuts carry theirs: a circle in the runner's colour with a white outline and the runner's mark in white. The marks
# are the official Claude symbol and Google Gemini sparkle (path data from their Wikimedia Commons files,
# Claude_AI_symbol.svg and Google_Gemini_icon_2025.svg), trademarks of Anthropic and Google, used only for this
# machine's own launcher icons and shipped in nothing. The icons are rendered per machine with resvg-py and Pillow in
# the Windows Python; they are not a dependency of the codec, its tests or its tooling. The badge carries its mark from
# 32 pixels up and its colour alone below that.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'

$here = (Resolve-Path -LiteralPath $PSScriptRoot).ProviderPath
if ($here -notmatch '^\\\\(?:wsl\.localhost|wsl\$)\\([^\\]+)\\(.+)\\scripts$') {
  throw "Run this from the WSL checkout (\\wsl.localhost\<distro>\...\scripts), not from $here."
}
$distro = $Matches[1]
$linuxRepo = '/' + ($Matches[2] -replace '\\', '/')
$repo = Split-Path -Parent $here
if ("$distro$linuxRepo" -match '\s') {
  throw "The distro or checkout path contains whitespace ($distro, $linuxRepo): wsl.exe cannot take it from a shortcut. Move the checkout to a path without spaces."
}

$render = @'
import io, os, sys
import resvg_py
from PIL import Image, ImageDraw
MARKS = {
    "codex": ('1.68 1.75 16.65 16.5', 'M11.248 18.25q-.825 0-1.568-.314a4.3 4.3 0 0 1-1.32-.874 4 4 0 0 1-1.304.214 4 4 0 0 1-2.046-.544 4.27 4.27 0 0 1-1.518-1.485 4 4 0 0 1-.56-2.095q0-.48.131-1.04A4.4 4.4 0 0 1 2.04 10.71a4.07 4.07 0 0 1 .017-3.4 4.2 4.2 0 0 1 1.056-1.418 3.8 3.8 0 0 1 1.6-.842 3.9 3.9 0 0 1 .76-1.683q.593-.759 1.451-1.188a4.04 4.04 0 0 1 1.832-.429q.825 0 1.567.313.742.314 1.32.875a4 4 0 0 1 1.304-.215q1.106 0 2.046.545a4.14 4.14 0 0 1 1.501 1.485q.578.941.578 2.095 0 .48-.132 1.04.66.61 1.023 1.419.363.792.363 1.666 0 .892-.38 1.717a4.3 4.3 0 0 1-1.072 1.435 3.8 3.8 0 0 1-1.584.825 3.8 3.8 0 0 1-.775 1.683 4.06 4.06 0 0 1-1.436 1.188 4.04 4.04 0 0 1-1.832.429m-4.076-2.062q.825 0 1.435-.347l3.103-1.782a.36.36 0 0 0 .164-.313v-1.42L7.881 14.62a.67.67 0 0 1-.726 0l-3.118-1.798a.5.5 0 0 1-.017.115v.198q0 .841.396 1.551.413.693 1.139 1.089a3.2 3.2 0 0 0 1.617.412m.165-2.69a.4.4 0 0 0 .181.05q.083 0 .165-.05l1.238-.71-3.977-2.31a.7.7 0 0 1-.363-.643v-3.58q-.825.362-1.32 1.122a2.9 2.9 0 0 0-.495 1.65q0 .809.413 1.55.412.743 1.072 1.123zm3.91 3.663q.875 0 1.585-.396a2.96 2.96 0 0 0 1.534-2.64v-3.564a.32.32 0 0 0-.165-.297l-1.254-.726v4.604a.7.7 0 0 1-.363.643l-3.119 1.799a3 3 0 0 0 1.783.577m.627-6.039V8.878L10.01 7.822 8.129 8.878v2.244l1.881 1.056zM7.057 5.859a.7.7 0 0 1 .363-.644l3.119-1.798a3 3 0 0 0-1.782-.578q-.874 0-1.584.396A2.96 2.96 0 0 0 6.05 4.324a3.07 3.07 0 0 0-.396 1.551v3.547q0 .199.165.314l1.237.726zm8.383 7.887q.825-.364 1.303-1.123.495-.758.495-1.65a3.15 3.15 0 0 0-.412-1.55q-.413-.743-1.073-1.123l-3.086-1.782q-.099-.065-.181-.049a.3.3 0 0 0-.165.05l-1.238.692 3.993 2.327a.6.6 0 0 1 .264.264.64.64 0 0 1 .1.363zm-3.317-8.382a.63.63 0 0 1 .726 0l3.135 1.831v-.297q0-.792-.396-1.501a2.86 2.86 0 0 0-1.105-1.155q-.71-.43-1.65-.43-.825 0-1.436.347L8.294 5.941a.36.36 0 0 0-.165.314v1.418z'),
    "claude": ("0 0 100 100", "m19.6 66.5 19.7-11 .3-1-.3-.5h-1l-3.3-.2-11.2-.3L14 53l-9.5-.5-2.4-.5L0 49l.2-1.5 2-1.3 2.9.2 6.3.5 9.5.6 6.9.4L38 49.1h1.6l.2-.7-.5-.4-.4-.4L29 41l-10.6-7-5.6-4.1-3-2-1.5-2-.6-4.2 2.7-3 3.7.3.9.2 3.7 2.9 8 6.1L37 36l1.5 1.2.6-.4.1-.3-.7-1.1L33 25l-6-10.4-2.7-4.3-.7-2.6c-.3-1-.4-2-.4-3l3-4.2L28 0l4.2.6L33.8 2l2.6 6 4.1 9.3L47 29.9l2 3.8 1 3.4.3 1h.7v-.5l.5-7.2 1-8.7 1-11.2.3-3.2 1.6-3.8 3-2L61 2.6l2 2.9-.3 1.8-1.1 7.7L59 27.1l-1.5 8.2h.9l1-1.1 4.1-5.4 6.9-8.6 3-3.5L77 13l2.3-1.8h4.3l3.1 4.7-1.4 4.9-4.4 5.6-3.7 4.7-5.3 7.1-3.2 5.7.3.4h.7l12-2.6 6.4-1.1 7.6-1.3 3.5 1.6.4 1.6-1.4 3.4-8.2 2-9.6 2-14.3 3.3-.2.1.2.3 6.4.6 2.8.2h6.8l12.6 1 3.3 2 1.9 2.7-.3 2-5.1 2.6-6.8-1.6-16-3.8-5.4-1.3h-.8v.4l4.6 4.5 8.3 7.5L89 80.1l.5 2.4-1.3 2-1.4-.2-9.2-7-3.6-3-8-6.8h-.5v.7l1.8 2.7 9.8 14.7.5 4.5-.7 1.4-2.6 1-2.7-.6-5.8-8-6-9-4.7-8.2-.5.4-2.9 30.2-1.3 1.5-3 1.2-2.5-2-1.4-3 1.4-6.2 1.6-8 1.3-6.4 1.2-7.9.7-2.6v-.2H49L43 72l-9 12.3-7.2 7.6-1.7.7-3-1.5.3-2.8L24 86l10-12.8 6-7.9 4-4.6-.1-.5h-.3L17.2 77.4l-4.7.6-2-2 .2-3 1-1 8-5.5Z"),
    "gemini": ("0 0 65 65", "M32.447 0c.68 0 1.273.465 1.439 1.125a38.904 38.904 0 001.999 5.905c2.152 5 5.105 9.376 8.854 13.125 3.751 3.75 8.126 6.703 13.125 8.855a38.98 38.98 0 005.906 1.999c.66.166 1.124.758 1.124 1.438 0 .68-.464 1.273-1.125 1.439a38.902 38.902 0 00-5.905 1.999c-5 2.152-9.375 5.105-13.125 8.854-3.749 3.751-6.702 8.126-8.854 13.125a38.973 38.973 0 00-2 5.906 1.485 1.485 0 01-1.438 1.124c-.68 0-1.272-.464-1.438-1.125a38.913 38.913 0 00-2-5.905c-2.151-5-5.103-9.375-8.854-13.125-3.75-3.749-8.125-6.702-13.125-8.854a38.973 38.973 0 00-5.905-2A1.485 1.485 0 010 32.448c0-.68.465-1.272 1.125-1.438a38.903 38.903 0 005.905-2c5-2.151 9.376-5.104 13.125-8.854 3.75-3.749 6.703-8.125 8.855-13.125a38.972 38.972 0 001.999-5.905A1.485 1.485 0 0132.447 0z"),
}
svg, target, fill, mark = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
sizes = (16, 20, 24, 30, 32, 40, 48, 64, 128, 256)
def png(**kw):
    return Image.open(io.BytesIO(bytes(resvg_py.svg_to_bytes(**kw)))).convert("RGBA")
def frame(size):
    tile = png(svg_path=svg, width=size, height=size)
    scale = 4  # drawn large and reduced, so the badge's edge is smooth at every size
    big = Image.new("RGBA", (size * scale, size * scale), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    r = round(size * scale * 0.24)
    cx = cy = size * scale - r - max(1, round(size * scale * 0.01))
    ring = max(scale, round(size * scale * 0.025))
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill="white")
    d.ellipse((cx - r + ring, cy - r + ring, cx + r - ring, cy + r - ring), fill=fill)
    if size >= 32:
        box, path = MARKS[mark]
        side = round(r * 1.15)
        logo = png(svg_string=f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{box}"><path fill="#fff" d="{path}"/></svg>',
                   width=side, height=side)
        big.alpha_composite(logo, (cx - side // 2, cy - side // 2))
    tile.alpha_composite(big.resize((size, size), Image.LANCZOS))
    return tile
frames = [frame(size) for size in sizes]
frames[-1].save(target + ".tmp", format="ICO", sizes=[(n, n) for n in sizes], append_images=frames[:-1])
os.replace(target + ".tmp", target)
'@

$desktop = [Environment]::GetFolderPath('Desktop')
$icons = Join-Path $env:LOCALAPPDATA 'RasterFold'
New-Item -ItemType Directory -Force $icons | Out-Null
$svg = Join-Path $repo 'assets\brand\icon.svg'
$wsl = Join-Path $env:SystemRoot 'System32\wsl.exe'
$shortcuts = @(
  @{ Name = 'RasterFold Claude campaign'; Runner = ''; Icon = 'rasterfold-campaign-claude.ico'; Fill = '#D97757'; Mark = 'claude'
     Description = 'Start or resume the unattended RasterFold campaign in Claude Code (latest Opus, medium) on main' },
  @{ Name = 'RasterFold Carbon campaign'; Runner = ' carbon'; Icon = 'rasterfold-campaign-carbon.ico'; Fill = '#4C8DF6'; Mark = 'gemini'
     Description = 'Start or resume the unattended RasterFold campaign in Google Antigravity on Orchestrator (Carbon) on main' },
  @{ Name = 'RasterFold Codex campaign'; Runner = ' codex'; Icon = 'rasterfold-campaign-codex.ico'; Fill = '#10A37F'; Mark = 'codex'
     Description = 'Start or resume RasterFold with Codex as writer on main' }
)
foreach ($s in $shortcuts) {
  $icon = Join-Path $icons $s.Icon
  $render | python - $svg $icon $s.Fill $s.Mark
  if ($LASTEXITCODE -ne 0) { throw 'The shortcut icon could not be rendered: install resvg-py and Pillow into the Windows Python (python -m pip install resvg-py Pillow), then run this again.' }
  $link = Join-Path $desktop "$($s.Name).lnk"
  $shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut($link)
  $shortcut.TargetPath = $wsl
  # Unquoted on purpose: wsl.exe started from a shortcut runs nothing when -d or --cd is quoted (measured
  # 2026-11-12 with two probe shortcuts that differed only in the quotes), so a path with spaces is refused above.
  $shortcut.Arguments = "-d $distro --cd $linuxRepo -- bash scripts/unattended-run.sh$($s.Runner)"
  $shortcut.WorkingDirectory = $env:USERPROFILE
  $shortcut.IconLocation = "$icon,0"
  $shortcut.Description = $s.Description
  $shortcut.Save()
  Write-Host "Shortcut written: $link -> wsl.exe $($shortcut.Arguments)"
}
# The single shortcut written before the runners split (2026-11-12): it is now "RasterFold Claude campaign".
$old = Join-Path $desktop 'RasterFold Unattended Run.lnk'
if (Test-Path -LiteralPath $old) { Remove-Item -LiteralPath $old; Write-Host "Removed: $old" }

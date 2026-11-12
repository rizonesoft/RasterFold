"""Build the research corpus in build/research/ppm and its JPEG XL e7, e9, and WebP reference sizes.

  python research/corpus.py

Downloads the 24 Kodak images (r0k.us) and the imagecompression.info 8-bit RGB
set (346 MB, once), cuts 1280x960 center crops of the latter, converts
measure/image.bmp, the test pack in testdata/photos and the generated images of
tests/benchmark.py to PPM, then encodes every image with ffmpeg libjxl (effort 7
and effort 9) and lossless libwebp and records the sizes in jxl_refs.json.
Everything lands in build/research, which git ignores.
"""

from __future__ import annotations

import io
import json
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[1]
DATA = REPO / "build" / "research"
PPM = DATA / "ppm"
sys.path.insert(0, str(REPO / "tests"))

# Held-out photographs: Wikimedia Commons featured pictures (Pictures of the
# Day are drawn from featured pictures), pinned at authoring on 2026-11-09.
# Licence, author, and page were verified against the Commons API
# (imageinfo/extmetadata) at authoring; the frozen manifest in
# research/heldout.json is the record later runs verify against.
HELDOUT_PHOTOS = [
    {
        "name": "ho_p01",
        "title": "014 Wild Red Deer Switzerland Photo by Giles Laurent.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:014_Wild_Red_Deer_Switzerland_Photo_by_Giles_Laurent.jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Giles Laurent",
        "source_revision": 1279771862,
        "download": "https://upload.wikimedia.org/wikipedia/commons/6/66/014_Wild_Red_Deer_Switzerland_Photo_by_Giles_Laurent.jpg"
    },
    {
        "name": "ho_p02",
        "title": "13-04-13-st-poelten-landhausviertel-628.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:13-04-13-st-poelten-landhausviertel-628.jpg",
        "licence": "CC BY 3.0",
        "licence_url": "https://creativecommons.org/licenses/by/3.0",
        "artist": "Ralf Roletschek",
        "source_revision": 1159208277,
        "download": "https://upload.wikimedia.org/wikipedia/commons/d/dc/13-04-13-st-poelten-landhausviertel-628.jpg"
    },
    {
        "name": "ho_p03",
        "title": "160316-031 Bridge near Tatopani.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:160316-031_Bridge_near_Tatopani.jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Faj2323",
        "source_revision": 1279785633,
        "download": "https://upload.wikimedia.org/wikipedia/commons/2/2d/160316-031_Bridge_near_Tatopani.jpg"
    },
    {
        "name": "ho_p04",
        "title": "2014 Kłodzko, starówka 12.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:2014_K%C5%82odzko,_star%C3%B3wka_12.jpg",
        "licence": "CC BY-SA 3.0 pl",
        "licence_url": "https://creativecommons.org/licenses/by-sa/3.0/pl/deed.en",
        "artist": "Jacek Halicki",
        "source_revision": 1237945286,
        "download": "https://upload.wikimedia.org/wikipedia/commons/5/5b/2014_K%C5%82odzko%2C_star%C3%B3wka_12.jpg"
    },
    {
        "name": "ho_p05",
        "title": "A Bukharan dance performed by members of the Rina Nikova ballet in the citadel of Jerusalem - Restoration.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:A_Bukharan_dance_performed_by_members_of_the_Rina_Nikova_ballet_in_the_citadel_of_Jerusalem_-_Restoration.jpg",
        "licence": "Public domain",
        "licence_url": "https://commons.wikimedia.org/wiki/Commons:Public_domain",
        "artist": "Zoltan Kluger / Adam Cuerden",
        "source_revision": 996216388,
        "download": "https://upload.wikimedia.org/wikipedia/commons/8/88/A_Bukharan_dance_performed_by_members_of_the_Rina_Nikova_ballet_in_the_citadel_of_Jerusalem_-_Restoration.jpg"
    },
    {
        "name": "ho_p06",
        "title": "Afgevallen kastanje van een Tamme kastanje (Castanea sativa) 17-10-2021 (d.j.b.) 01.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Afgevallen_kastanje_van_een_Tamme_kastanje_(Castanea_sativa)_17-10-2021_(d.j.b.)_01.jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Dominicus Johannes Bergsma",
        "source_revision": 1283140152,
        "download": "https://upload.wikimedia.org/wikipedia/commons/a/af/Afgevallen_kastanje_van_een_Tamme_kastanje_%28Castanea_sativa%29_17-10-2021_%28d.j.b.%29_01.jpg"
    },
    {
        "name": "ho_p07",
        "title": "Amaryllis (Hippeastrum) 26-01-2021. (actm.) 05.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Amaryllis_(Hippeastrum)_26-01-2021._(actm.)_05.jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Agnes Monkelbaan",
        "source_revision": 1281061212,
        "download": "https://upload.wikimedia.org/wikipedia/commons/f/f6/Amaryllis_%28Hippeastrum%29_26-01-2021._%28actm.%29_05.jpg"
    },
    {
        "name": "ho_p08",
        "title": "Ariramba-de-cauda-ruiva (cropped).jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Ariramba-de-cauda-ruiva_(cropped).jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Jairmoreirafotografia",
        "source_revision": 1255978386,
        "download": "https://upload.wikimedia.org/wikipedia/commons/e/ef/Ariramba-de-cauda-ruiva_%28cropped%29.jpg"
    },
    {
        "name": "ho_p09",
        "title": "Breil-Brigels, Lag da Breil- Flem. 23-09-2022. (actm.) 11.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Breil-Brigels,_Lag_da_Breil-_Flem._23-09-2022._(actm.)_11.jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Agnes Monkelbaan",
        "source_revision": 1178999879,
        "download": "https://upload.wikimedia.org/wikipedia/commons/d/df/Breil-Brigels%2C_Lag_da_Breil-_Flem._23-09-2022._%28actm.%29_11.jpg"
    },
    {
        "name": "ho_p10",
        "title": "Cappuchin Bridge (30706076020).jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Cappuchin_Bridge_(30706076020).jpg",
        "licence": "CC BY 2.0",
        "licence_url": "https://creativecommons.org/licenses/by/2.0",
        "artist": "Bernd Thaller from Graz, Austria",
        "source_revision": 1113601838,
        "download": "https://upload.wikimedia.org/wikipedia/commons/c/cf/Cappuchin_Bridge_%2830706076020%29.jpg"
    },
    {
        "name": "ho_p11",
        "title": "Catedral de Santa María, Sigüenza, España, 2015-12-28, DD 118-120 HDR.JPG",
        "page": "https://commons.wikimedia.org/wiki/File:Catedral_de_Santa_Mar%C3%ADa,_Sig%C3%BCenza,_Espa%C3%B1a,_2015-12-28,_DD_118-120_HDR.JPG",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Diego Delso",
        "source_revision": 1110838213,
        "download": "https://upload.wikimedia.org/wikipedia/commons/a/a0/Catedral_de_Santa_Mar%C3%ADa%2C_Sig%C3%BCenza%2C_Espa%C3%B1a%2C_2015-12-28%2C_DD_118-120_HDR.JPG"
    },
    {
        "name": "ho_p12",
        "title": "Church of St. John at Kaneo 6.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Church_of_St._John_at_Kaneo_6.jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "kallerna",
        "source_revision": 1261675782,
        "download": "https://upload.wikimedia.org/wikipedia/commons/4/4a/Church_of_St._John_at_Kaneo_6.jpg"
    },
    {
        "name": "ho_p13",
        "title": "Coccinella in Parc du Bois-de-Coulonge uncut version.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Coccinella_in_Parc_du_Bois-de-Coulonge_uncut_version.jpg",
        "licence": "CC0",
        "licence_url": "http://creativecommons.org/publicdomain/zero/1.0/deed.en",
        "artist": "Wilfredor",
        "source_revision": 1281145559,
        "download": "https://upload.wikimedia.org/wikipedia/commons/2/29/Coccinella_in_Parc_du_Bois-de-Coulonge_uncut_version.jpg"
    },
    {
        "name": "ho_p14",
        "title": "Czech Republic - landscape near Koryčany.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Czech_Republic_-_landscape_near_Kory%C4%8Dany.jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Pudelek",
        "source_revision": 1154699554,
        "download": "https://upload.wikimedia.org/wikipedia/commons/2/27/Czech_Republic_-_landscape_near_Kory%C4%8Dany.jpg"
    },
    {
        "name": "ho_p15",
        "title": "Dülmen, Heilig-Kreuz-Kirche, The Cast Whale Project -- 2022 -- 0509-13.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:D%C3%BClmen,_Heilig-Kreuz-Kirche,_The_Cast_Whale_Project_--_2022_--_0509-13.jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Dietmar Rabich",
        "source_revision": 1296635051,
        "download": "https://upload.wikimedia.org/wikipedia/commons/1/11/D%C3%BClmen%2C_Heilig-Kreuz-Kirche%2C_The_Cast_Whale_Project_--_2022_--_0509-13.jpg"
    },
    {
        "name": "ho_p16",
        "title": "Dülmen, Münsterstraße, Viktorkirmes, Riesenrad -- 2023 -- 9053 (kreativ).jpg",
        "page": "https://commons.wikimedia.org/wiki/File:D%C3%BClmen,_M%C3%BCnsterstra%C3%9Fe,_Viktorkirmes,_Riesenrad_--_2023_--_9053_(kreativ).jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Dietmar Rabich",
        "source_revision": 1297670992,
        "download": "https://upload.wikimedia.org/wikipedia/commons/f/f9/D%C3%BClmen%2C_M%C3%BCnsterstra%C3%9Fe%2C_Viktorkirmes%2C_Riesenrad_--_2023_--_9053_%28kreativ%29.jpg"
    },
    {
        "name": "ho_p17",
        "title": "Edinburgh Castle from Grass Market.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Edinburgh_Castle_from_Grass_Market.jpg",
        "licence": "Public domain",
        "licence_url": "https://commons.wikimedia.org/wiki/Commons:Public_domain",
        "artist": "George Washington Wilson / Adam Cuerden",
        "source_revision": 1223836504,
        "download": "https://upload.wikimedia.org/wikipedia/commons/c/c4/Edinburgh_Castle_from_Grass_Market.jpg"
    },
    {
        "name": "ho_p18",
        "title": "Ethiopia Banna tribe kids.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Ethiopia_Banna_tribe_kids.jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "WAVRIK",
        "source_revision": 1277542930,
        "download": "https://upload.wikimedia.org/wikipedia/commons/5/5a/Ethiopia_Banna_tribe_kids.jpg"
    },
    {
        "name": "ho_p19",
        "title": "Five children on a motorcycle.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Five_children_on_a_motorcycle.jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Basile Morin",
        "source_revision": 1294128498,
        "download": "https://upload.wikimedia.org/wikipedia/commons/4/41/Five_children_on_a_motorcycle.jpg"
    },
    {
        "name": "ho_p20",
        "title": "Gele trilzwam (Tremella mesenterica) op dode tak van een eik 15-01-2021. (actm.) 02.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Gele_trilzwam_(Tremella_mesenterica)_op_dode_tak_van_een_eik_15-01-2021._(actm.)_02.jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Agnes Monkelbaan",
        "source_revision": 1281651901,
        "download": "https://upload.wikimedia.org/wikipedia/commons/4/42/Gele_trilzwam_%28Tremella_mesenterica%29_op_dode_tak_van_een_eik_15-01-2021._%28actm.%29_02.jpg"
    },
    {
        "name": "ho_p21",
        "title": "Golden Delicious apples.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Golden_Delicious_apples.jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Ivar Leidus",
        "source_revision": 1275156055,
        "download": "https://upload.wikimedia.org/wikipedia/commons/0/0f/Golden_Delicious_apples.jpg"
    },
    {
        "name": "ho_p22",
        "title": "Habibi2006prצילום חביבי - יוסי צבקר.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Habibi2006pr%D7%A6%D7%99%D7%9C%D7%95%D7%9D_%D7%97%D7%91%D7%99%D7%91%D7%99_-_%D7%99%D7%95%D7%A1%D7%99_%D7%A6%D7%91%D7%A7%D7%A8.jpg",
        "licence": "CC BY-SA 3.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/3.0",
        "artist": "Yossi Zwecker",
        "source_revision": 1290524574,
        "download": "https://upload.wikimedia.org/wikipedia/commons/c/c5/Habibi2006pr%D7%A6%D7%99%D7%9C%D7%95%D7%9D_%D7%97%D7%91%D7%99%D7%91%D7%99_-_%D7%99%D7%95%D7%A1%D7%99_%D7%A6%D7%91%D7%A7%D7%A8.jpg"
    },
    {
        "name": "ho_p23",
        "title": "Herding sheep353 (edited).jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Herding_sheep353_(edited).jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Original:  Azonesa\nDerivative work:  Radomianin",
        "source_revision": 1296896671,
        "download": "https://upload.wikimedia.org/wikipedia/commons/f/f3/Herding_sheep353_%28edited%29.jpg"
    },
    {
        "name": "ho_p24",
        "title": "Hue Vietnam Thien-Mu-Temple-and-Pagoda-03.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Hue_Vietnam_Thien-Mu-Temple-and-Pagoda-03.jpg",
        "licence": "CC BY-SA 3.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/3.0",
        "artist": "CEphoto, Uwe Aranas",
        "source_revision": 1204446923,
        "download": "https://upload.wikimedia.org/wikipedia/commons/8/88/Hue_Vietnam_Thien-Mu-Temple-and-Pagoda-03.jpg"
    },
    {
        "name": "ho_p25",
        "title": "Katholische Pfarrkirche St. Julitta und Quiricus, Andiast. (actm) 06.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Katholische_Pfarrkirche_St._Julitta_und_Quiricus,_Andiast._(actm)_06.jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Agnes Monkelbaan",
        "source_revision": 1178999246,
        "download": "https://upload.wikimedia.org/wikipedia/commons/3/3f/Katholische_Pfarrkirche_St._Julitta_und_Quiricus%2C_Andiast._%28actm%29_06.jpg"
    },
    {
        "name": "ho_p26",
        "title": "Korfu (GR), Korfu, Alte Festung -- 2018 -- 1137.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Korfu_(GR),_Korfu,_Alte_Festung_--_2018_--_1137.jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Dietmar Rabich",
        "source_revision": 1270874735,
        "download": "https://upload.wikimedia.org/wikipedia/commons/3/34/Korfu_%28GR%29%2C_Korfu%2C_Alte_Festung_--_2018_--_1137.jpg"
    },
    {
        "name": "ho_p27",
        "title": "Lahad-Datu Sabah Mount-Silam-Red-Crab-Geosesarma-aurantium-02.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Lahad-Datu_Sabah_Mount-Silam-Red-Crab-Geosesarma-aurantium-02.jpg",
        "licence": "CC BY-SA 3.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/3.0",
        "artist": "CEphoto, Uwe Aranas",
        "source_revision": 1239631490,
        "download": "https://upload.wikimedia.org/wikipedia/commons/9/9a/Lahad-Datu_Sabah_Mount-Silam-Red-Crab-Geosesarma-aurantium-02.jpg"
    },
    {
        "name": "ho_p28",
        "title": "Mountains of Wadi Shawka denoised.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Mountains_of_Wadi_Shawka_denoised.jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "Original:  Florian Kriechbaumer\nDerivative work:  Aristeas",
        "source_revision": 1082913674,
        "download": "https://upload.wikimedia.org/wikipedia/commons/5/59/Mountains_of_Wadi_Shawka_denoised.jpg"
    },
    {
        "name": "ho_p29",
        "title": "Male Bush Elephant Head Trunk Up Kafue Jul23 A7R 05195.jpg",
        "page": "https://commons.wikimedia.org/wiki/File:Male_Bush_Elephant_Head_Trunk_Up_Kafue_Jul23_A7R_05195.jpg",
        "licence": "CC BY-SA 4.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/4.0",
        "artist": "This Photo was taken by Timothy A. Gonsalves.  Feel free to use my photos, but please mention me as the author.  I would much appreciate if you send me an email",
        "source_revision": 1286690103,
        "download": "https://upload.wikimedia.org/wikipedia/commons/1/1c/Male_Bush_Elephant_Head_Trunk_Up_Kafue_Jul23_A7R_05195.jpg"
    },
    {
        "name": "ho_p30",
        "title": "Mercado de Colón, Valencia, España, 2014-06-29, DD 07.JPG",
        "page": "https://commons.wikimedia.org/wiki/File:Mercado_de_Col%C3%B3n,_Valencia,_Espa%C3%B1a,_2014-06-29,_DD_07.JPG",
        "licence": "CC BY-SA 3.0",
        "licence_url": "https://creativecommons.org/licenses/by-sa/3.0",
        "artist": "Diego Delso",
        "source_revision": 1068292507,
        "download": "https://upload.wikimedia.org/wikipedia/commons/a/ab/Mercado_de_Col%C3%B3n%2C_Valencia%2C_Espa%C3%B1a%2C_2014-06-29%2C_DD_07.JPG"
    }
]

HELDOUT_PAGES = ["index", "getting-started", "installation", "usage", "examples",
                 "format", "c-api", "benchmarks", "limitations", "troubleshooting"]
HELDOUT_MANIFEST = REPO / "research" / "heldout.json"
# The development screenshots (D01 T07 section 1): the same renderer as the held-out screenshots, on pages that
# are not held out. Published pages come from the staged site, the two unpublished documents from docs/.
SCREENS_PAGES = [
    ("api", "build/site-source/api.md"),
    ("cli", "build/site-source/cli.md"),
    ("contributing", "build/site-source/contributing.md"),
    ("research-summary", "build/site-source/research-summary.md"),
    ("security", "build/site-source/security.md"),
    ("licenses", "build/site-source/licenses.md"),
    ("release-history", "build/site-source/release-history.md"),
    ("downloads", "build/site-source/downloads.md"),
    ("vision", "docs/vision.md"),
    ("codec-distribution", "docs/codec-distribution.md"),
]
SCREENS_MANIFEST = REPO / "research" / "screens.json"
SCREENS_ASSETS = REPO / "research" / "screens-assets"
SCREENS_SIZE = (1280, 960)

REFS_SPEC = (
    ("e7", ".e7.jxl", ["-c:v", "libjxl", "-distance", "0", "-effort", "7", "-threads", "1", "-update", "1"]),
    ("e9", ".e9.jxl", ["-c:v", "libjxl", "-distance", "0", "-effort", "9", "-threads", "1", "-update", "1"]),
    ("webp", ".webp", ["-c:v", "libwebp", "-lossless", "1", "-quality", "100", "-compression_level", "6", "-threads", "1", "-update", "1"]),
)


def is_positive_int(value: object) -> bool:
    return type(value) is int and value > 0


def fetch(url: str, dest: Path) -> None:
    if not dest.exists():
        print("download", url, flush=True)
        dest.parent.mkdir(parents=True, exist_ok=True)
        urllib.request.urlretrieve(url, dest)


def _ffmpeg_arg(exe: str, path: Path, runner=subprocess.run) -> str:
    if runner is subprocess.run and os.name != "nt" and exe.lower().endswith(".exe"):
        return subprocess.check_output(["wslpath", "-w", str(path)], text=True).strip()
    return str(path)


def encode_ref(
    ppm_dir: Path,
    jxl_dir: Path,
    name: str,
    kind: str,
    ext: str,
    args: list[str],
    runner=subprocess.run,
) -> tuple[str, int]:
    src = ppm_dir / f"{name}.ppm"
    dest = jxl_dir / f"{name}{ext}"
    exe = shutil.which("ffmpeg") or shutil.which("ffmpeg.exe") or "ffmpeg"
    cmd = [
        exe,
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        _ffmpeg_arg(exe, src, runner),
        *args,
        _ffmpeg_arg(exe, dest, runner),
    ]
    r = runner(cmd, capture_output=True)
    if r.returncode != 0 or not dest.is_file():
        err = (r.stderr or b"").decode("utf-8", "replace").strip() if isinstance(r.stderr, bytes) else str(r.stderr or "").strip()
        raise RuntimeError(f"ffmpeg failed for {name}:{kind} (exit {r.returncode}): {err}")
    size = dest.stat().st_size
    if not is_positive_int(size):
        raise RuntimeError(f"ffmpeg wrote non-positive size for {name}:{kind}: {size}")
    return f"{name}:{kind}", size


def validate_refs(refs: dict, stems: list[str]) -> None:
    if not isinstance(refs, dict):
        raise ValueError("jxl_refs.json root must be a JSON object")
    for stem in stems:
        for kind, _ext, _args in REFS_SPEC:
            key = f"{stem}:{kind}"
            if key not in refs:
                raise ValueError(f"missing reference in jxl_refs.json: {key}")
            if not is_positive_int(refs[key]):
                raise ValueError(f"invalid reference in jxl_refs.json: {key}={refs[key]!r}")


def update_refs(
    ppm_dir: Path = PPM,
    jxl_dir: Path = DATA / "jxl",
    refs_path: Path = DATA / "jxl_refs.json",
    runner=subprocess.run,
) -> dict[str, int]:
    jxl_dir.mkdir(parents=True, exist_ok=True)
    raw = json.loads(refs_path.read_text(encoding="utf-8")) if refs_path.exists() else {}
    if not isinstance(raw, dict):
        raise SystemExit("jxl_refs.json root must be a JSON object")
    refs: dict[str, int] = {k: v for k, v in raw.items() if isinstance(k, str) and is_positive_int(v)}
    stems = sorted(p.stem for p in ppm_dir.glob("*.ppm"))
    if not stems:
        raise SystemExit(f"no PPM images in {ppm_dir}")
    todo = [
        (stem, kind, ext, list(args))
        for stem in stems
        for kind, ext, args in REFS_SPEC
        if not is_positive_int(refs.get(f"{stem}:{kind}"))
    ]

    def _one(task: tuple[str, str, str, list[str]]) -> tuple[str, int]:
        stem, kind, ext, args = task
        return encode_ref(ppm_dir, jxl_dir, stem, kind, ext, args, runner=runner)

    if todo:
        try:
            with ThreadPoolExecutor(6) as pool:
                for key, size in pool.map(_one, todo):
                    refs[key] = size
                    print(key, size, flush=True)
        except Exception as exc:
            raise SystemExit(str(exc)) from exc
    try:
        validate_refs(refs, stems)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc
    refs_path.parent.mkdir(parents=True, exist_ok=True)
    refs_path.write_text(json.dumps(refs, indent=1, sort_keys=True), encoding="utf-8")
    reloaded = json.loads(refs_path.read_text(encoding="utf-8"))
    try:
        validate_refs(reloaded, stems)
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc
    return reloaded




def _rgb_sha256(im) -> str:
    import hashlib
    return hashlib.sha256(im.convert("RGB").tobytes()).hexdigest()


def _download_validated(url: str, dest: Path) -> bytes:
    """Bounded download to a temporary file, validated as a large image, then published."""
    if dest.is_file():
        try:
            with Image.open(dest) as im:
                im.load()
                if im.width >= 1280 and im.height >= 960:
                    return dest.read_bytes()
        except Exception:
            pass
    data = None
    for attempt in range(5):
        try:
            import subprocess
            import time as _time
            # The transfer runs in a child process the parent waits on
            # with the 300-second deadline and terminates on expiry. No
            # in-process read pattern enforces a total bound (buffered
            # and chunk-framing reads perform many receives per call, and
            # a thread blocked in connect()/DNS cannot be woken), while
            # process termination cancels every stage at once and the
            # operating system reclaims the child's sockets, so cleanup
            # is owned by the worker's exit, never by shared state.
            fetch = (
                "import sys, urllib.request\n"
                "url, dest = sys.argv[1], sys.argv[2]\n"
                "req = urllib.request.Request(url, headers={'User-Agent': 'RasterFold-research/1.0 (held-out set)'})\n"
                "total = 0\n"
                "with urllib.request.urlopen(req, timeout=60) as r, open(dest, 'wb') as f:\n"
                "    while True:\n"
                "        chunk = r.read(65536)\n"
                "        if not chunk: break\n"
                "        total += len(chunk)\n"
                "        if total > 96 * 1024 * 1024: sys.exit('heldout download exceeds 96 MiB')\n"
                "        f.write(chunk)\n"
            )
            part = dest.with_suffix(".part")
            proc = subprocess.Popen(
                [sys.executable, "-B", "-c", fetch, url, str(part)],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            try:
                _, err_text = proc.communicate(timeout=300)
            except subprocess.TimeoutExpired:
                part.unlink(missing_ok=True)
                raise SystemExit(f"heldout download exceeds 300s for {url}")
            finally:
                # Every exit, timeout, interruption of the caller
                # (KeyboardInterrupt), or error, terminates and reaps the
                # child: the transfer never outlives its parent. The 300
                # seconds bound the transfer; termination and reaping may
                # add up to five seconds before the kill escalation.
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
            if proc.returncode != 0:
                part.unlink(missing_ok=True)
                raise SystemExit(f"heldout download failed for {url}: {err_text.strip()}")
            data = part.read_bytes()
            part.unlink(missing_ok=True)
            break
        except Exception as exc:
            if attempt == 4:
                raise SystemExit(f"heldout download failed for {url}: {exc}") from exc
            import time as _time
            _time.sleep(15 * (attempt + 1))
    tmp = dest.with_suffix(".tmp")
    tmp.write_bytes(data)
    try:
        with Image.open(tmp) as im:
            im.load()
            if im.width < 1280 or im.height < 960:
                raise SystemExit(f"heldout source too small for {url}: {im.width}x{im.height}")
    except SystemExit:
        tmp.unlink(missing_ok=True)
        raise
    except Exception as exc:
        tmp.unlink(missing_ok=True)
        raise SystemExit(f"heldout download is not a valid image for {url}: {exc}") from exc
    tmp.replace(dest)
    return data


def _ppm_pixels(ppm_path: Path):
    data = ppm_path.read_bytes()
    parts = data.split(b"\n", 3)
    if len(parts) != 4 or parts[0] != b"P6":
        raise SystemExit(f"invalid PPM in {ppm_path}: bad magic")
    dims = parts[1].split()
    if len(dims) != 2:
        raise SystemExit(f"invalid PPM in {ppm_path}: bad dimensions")
    w, h = int(dims[0]), int(dims[1])
    if parts[2] != b"255":
        raise SystemExit(f"invalid PPM in {ppm_path}: maxval {parts[2]!r}, expected 255")
    payload = parts[3]
    if len(payload) != w * h * 3:
        raise SystemExit(f"invalid PPM in {ppm_path}: payload {len(payload)} bytes, expected {w * h * 3}")
    return w, h, payload


def verify_heldout(ppm_dir: Path = PPM) -> list[dict]:
    """Membership, dimensions, and hash verification against the frozen manifest. No regeneration."""
    import hashlib
    if not HELDOUT_MANIFEST.is_file():
        raise SystemExit("heldout manifest missing: research/heldout.json (run python research/corpus.py to author it)")
    manifest = json.loads(HELDOUT_MANIFEST.read_text(encoding="utf-8"))
    entries = manifest["images"]
    names = [e["name"] for e in entries]
    photos = [e for e in entries if e["kind"] == "photograph"]
    shots = [e for e in entries if e["kind"] == "screenshot"]
    if len(photos) < 30 or len(shots) < 10:
        raise SystemExit(f"heldout membership: {len(photos)} photographs, {len(shots)} screenshots (need 30 + 10)")
    if len(names) != len(set(names)):
        raise SystemExit("heldout membership: duplicate names in manifest")
    sys.path.insert(0, str(REPO / "research"))
    import ab
    if names != ab.SETS["heldout"]:
        raise SystemExit("heldout membership: manifest names do not match SETS['heldout'] in research/ab.py")
    for e in entries:
        ppm_path = ppm_dir / f"{e['name']}.ppm"
        if not ppm_path.is_file():
            raise SystemExit(f"heldout missing image: {e['name']}")
        w, h, pixels = _ppm_pixels(ppm_path)
        if (w, h) != (1280, 960):
            raise SystemExit(f"heldout {e['name']}: {w}x{h}, expected 1280x960")
        if hashlib.sha256(pixels).hexdigest() != e["rgb_sha256"]:
            raise SystemExit(f"heldout {e['name']}: RGB SHA-256 mismatch against the frozen manifest")
    print(f"heldout verified: {len(photos)} photographs, {len(shots)} screenshots, hashes match")
    return entries


def _provision_browser(browser_version: str) -> Path:
    """Download the pinned Chrome for Testing headless shell (recovery path).

    The recorded browser identity is a full `Google Chrome for Testing
    <x.y.z.w>` string; that exact build is fetched from the Chrome for
    Testing public archive into ignored build space and verified by its
    own `--version` output before use, so a fresh host recovers the frozen
    screenshots instead of failing on an empty browser cache.
    """
    import io
    import re as _re
    import urllib.request
    import zipfile
    m = _re.search(r"(\d+\.\d+\.\d+\.\d+)", browser_version)
    if not m:
        raise SystemExit(f"heldout screenshots: cannot parse a version from recorded browser {browser_version!r}")
    ver = m.group(1)
    root = REPO / "build" / "heldout-browser" / ver
    shell = root / "chrome-headless-shell-linux64" / "chrome-headless-shell"
    if not shell.is_file():
        url = f"https://storage.googleapis.com/chrome-for-testing-public/{ver}/linux64/chrome-headless-shell-linux64.zip"
        req = urllib.request.Request(url, headers={"User-Agent": "RasterFold-research/1.0 (held-out set)"})
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                blob = r.read()
        except Exception as exc:
            raise SystemExit(f"heldout screenshots: provisioning pinned browser {ver} failed: {exc}") from exc
        root.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(io.BytesIO(blob)) as z:
            z.extractall(root)
        shell.chmod(0o755)
    got = subprocess.run([str(shell), "--version"], capture_output=True, text=True).stdout.strip()
    if got != browser_version:
        raise SystemExit(f"heldout screenshots: provisioned browser reports {got!r}, recorded is {browser_version!r}; refusing to regenerate")
    return shell


def _screenshot_pages(browser_version: str | None = None, pages: list[tuple[str, str]] | None = None) -> list[dict]:
    """Render pages as 1280x960 screenshots. `pages` is a list of (page id, repository-relative Markdown path);
    the default is the held-out pages from the staged site."""
    stage = REPO / "build" / "site-source"
    subprocess.run([sys.executable, str(REPO / "scripts" / "build-docs.py"), "--stage", "build/site-source"],
                   cwd=REPO, check=True, capture_output=True, text=True)
    shells = sorted(Path.home().glob(".cache/ms-playwright/chromium_headless_shell-*/chrome-headless-shell-linux64/chrome-headless-shell"))
    shells += sorted((REPO / "build" / "heldout-browser").glob("*/chrome-headless-shell-linux64/chrome-headless-shell"))
    shell = version = None
    for cand in shells:
        v = subprocess.run([str(cand), "--version"], capture_output=True, text=True).stdout.strip()
        if browser_version is None or v == browser_version:
            shell, version = str(cand), v
            if browser_version is not None:
                break
    if shell is None and browser_version is not None:
        shell_path = _provision_browser(browser_version)
        shell, version = str(shell_path), browser_version
    if shell is None:
        raise SystemExit("heldout screenshots: no cached headless Chromium shell under ~/.cache/ms-playwright and no recorded browser version to provision")
    entries = []
    html_dir = REPO / "build" / "heldout-html"
    html_dir.mkdir(parents=True, exist_ok=True)
    if pages is None:
        pages = [(page, f"build/site-source/{page}.md") for page in HELDOUT_PAGES]
    for i, (page, md_rel) in enumerate(pages, 1):
        md = REPO / md_rel
        if not md.is_file():
            raise SystemExit(f"heldout screenshot source missing: {md}")
        r = subprocess.run(["npx", "-y", "markdown-it@14.1.0", str(md)],
                           capture_output=True, text=True, timeout=120)
        if r.returncode != 0:
            raise SystemExit(f"heldout markdown render failed for {page}: {r.stderr[:200]}")
        html = ("<!DOCTYPE html><html><head><meta charset='utf-8'><style>"
                "body{font-family:sans-serif;margin:24px;line-height:1.45;color:#111;background:#fff}"
                "pre,code{background:#f2f2f2}table{border-collapse:collapse}td,th{border:1px solid #999;padding:2px 8px}"
                "</style></head><body>" + r.stdout + "</body></html>")
        html_path = html_dir / f"{page}.html"
        html_path.write_text(html, encoding="utf-8")
        png = html_dir / f"{page}.png"
        r = subprocess.run([shell, "--headless", "--disable-gpu", "--hide-scrollbars",
                            "--window-size=1280,960", f"--screenshot={png}", f"file://{html_path}"],
                           capture_output=True, text=True, timeout=120)
        if r.returncode != 0 or not png.is_file():
            raise SystemExit(f"heldout screenshot failed for {page}: {r.stderr[:200]}")
        import hashlib as _hl
        entries.append({"page": page, "png": png, "browser": f"{shell} ({version})",
                        "source_sha256": _hl.sha256(md.read_bytes()).hexdigest()})
    return entries


def _ppm_rgb_sha256(ppm_path: Path) -> tuple[int, int, str]:
    import hashlib
    w, h, pixels = _ppm_pixels(ppm_path)
    return w, h, hashlib.sha256(pixels).hexdigest()


def verify_screens(ppm_dir: Path = PPM) -> list[dict]:
    """The development screenshots against their frozen manifest: membership, dimensions, RGB hashes, and no
    overlap with the held-out set. No regeneration."""
    if not SCREENS_MANIFEST.is_file():
        raise SystemExit("screens manifest missing: research/screens.json (author it in WSL with python3 research/corpus.py --author-screens)")
    manifest = json.loads(SCREENS_MANIFEST.read_text(encoding="utf-8"))
    entries = manifest["images"]
    sys.path.insert(0, str(REPO / "research"))
    import ab
    if [e["name"] for e in entries] != ab.SETS["screens"]:
        raise SystemExit("screens membership: manifest names do not match SETS['screens'] in research/ab.py")
    _refuse_heldout_overlap(entries)
    for e in entries:
        ppm_path = ppm_dir / f"{e['name']}.ppm"
        if not ppm_path.is_file():
            raise SystemExit(f"screens missing image: {e['name']}")
        w, h, digest = _ppm_rgb_sha256(ppm_path)
        if (w, h) != SCREENS_SIZE:
            raise SystemExit(f"screens {e['name']}: {w}x{h}, expected 1280x960")
        if digest != e["rgb_sha256"]:
            raise SystemExit(f"screens {e['name']}: RGB SHA-256 mismatch against the frozen manifest")
    print(f"screens verified: {len(entries)} screenshots, hashes match, no overlap with the held-out set")
    return entries


def _held_out_sources() -> set[str]:
    """Every repository path a held-out screenshot was rendered from: its staged page and the source that page
    is staged from (docs/site-content.json), normalized."""
    import posixpath
    held = json.loads(HELDOUT_MANIFEST.read_text(encoding="utf-8"))["images"]
    staged = {f"build/site-source/{page}.md" for page in HELDOUT_PAGES}
    staged |= {e["source_page"] for e in held if e.get("kind") == "screenshot"}
    site = json.loads((REPO / "docs" / "site-content.json").read_text(encoding="utf-8"))
    origin = {f"build/site-source/{p['staged']}": p.get("output") or p["source"] for p in site["pages"]}
    origin_src = {f"build/site-source/{p['staged']}": p["source"] for p in site["pages"]}
    sources = {posixpath.normpath(s) for s in staged}
    sources |= {posixpath.normpath(origin[s]) for s in staged if s in origin}
    sources |= {posixpath.normpath(origin_src[s]) for s in staged if s in origin_src}
    return sources


def _refuse_heldout_overlap(entries: list[dict]) -> None:
    """A development screenshot may share neither a source page nor its pixels with a held-out image. Pages are
    compared by label and by normalized source path (a staged page and the file it is staged from both count)."""
    import posixpath
    held = json.loads(HELDOUT_MANIFEST.read_text(encoding="utf-8"))["images"]
    held_pages = {page for page in HELDOUT_PAGES} | {
        e["source_page"].removeprefix("build/site-source/").removesuffix(".md") for e in held if e.get("kind") == "screenshot"}
    held_sources = _held_out_sources()
    held_hashes = {e["rgb_sha256"] for e in held}
    for e in entries:
        if e["page"] in held_pages:
            raise SystemExit(f"screens {e['name']}: page {e['page']!r} is a held-out page")
        if posixpath.normpath(e["source"]) in held_sources:
            raise SystemExit(f"screens {e['name']}: source {e['source']!r} is a held-out page's source")
        if e["rgb_sha256"] in held_hashes:
            raise SystemExit(f"screens {e['name']}: its pixels equal a held-out image")


def screens(ppm_dir: Path = PPM) -> None:
    """Recover any missing development PPM from its committed PNG, then verify the set. Never renders."""
    if not SCREENS_MANIFEST.is_file():
        print("screens: research/screens.json not authored yet; skipped")
        return
    manifest = json.loads(SCREENS_MANIFEST.read_text(encoding="utf-8"))
    for e in manifest["images"]:
        ppm_path = ppm_dir / f"{e['name']}.ppm"
        if ppm_path.is_file():
            continue
        asset = SCREENS_ASSETS / f"{e['name']}.png"
        if not asset.is_file():
            raise SystemExit(f"screens recovery: {e['name']}.ppm is missing and research/screens-assets/{e['name']}.png is not committed")
        with Image.open(asset) as im:
            im.convert("RGB").save(ppm_path)
    verify_screens(ppm_dir)


def author_screens(ppm_dir: Path = PPM) -> None:
    """Author the development screenshots, once, in WSL/Linux: render SCREENS_PAGES with the held-out set's
    recorded browser version, refuse any overlap with the held-out set, and write the PNG assets, the PPMs and the
    frozen manifest. With a manifest already present nothing is rewritten: a changed source page is refused, and an
    unchanged one is recovered and verified instead."""
    import hashlib
    import posixpath
    held_sources = _held_out_sources()
    for page, source in SCREENS_PAGES:
        if page in HELDOUT_PAGES:
            raise SystemExit(f"screens: page {page!r} is a held-out page")
        if posixpath.normpath(source) in held_sources:
            raise SystemExit(f"screens: source {source!r} is a held-out page's source")
    held = json.loads(HELDOUT_MANIFEST.read_text(encoding="utf-8"))["images"]
    versions = {e["browser_version"] for e in held if e.get("kind") == "screenshot"}
    if len(versions) != 1:
        raise SystemExit(f"screens: the held-out screenshots record {len(versions)} browser versions, expected one")
    (browser_version,) = versions
    subprocess.run([sys.executable, str(REPO / "scripts" / "build-docs.py"), "--stage", "build/site-source"],
                   cwd=REPO, check=True, capture_output=True, text=True)
    if SCREENS_MANIFEST.is_file():
        manifest = json.loads(SCREENS_MANIFEST.read_text(encoding="utf-8"))
        for e in manifest["images"]:
            md = REPO / e["source"]
            if not md.is_file() or hashlib.sha256(md.read_bytes()).hexdigest() != e["source_sha256"]:
                raise SystemExit(f"screens authoring: {e['source']} differs from the frozen source hash; the set is "
                                 "frozen and is not re-rendered from a changed page")
        screens(ppm_dir)
        return
    shots = _screenshot_pages(browser_version, SCREENS_PAGES)
    if any(s["browser"].rsplit("(", 1)[-1].removesuffix(")") != browser_version for s in shots):
        raise SystemExit(f"screens authoring: rendered with another browser than the held-out set's {browser_version!r}")
    SCREENS_ASSETS.mkdir(parents=True, exist_ok=True)
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO, capture_output=True, text=True).stdout.strip()
    entries = []
    for i, ((page, source), shot) in enumerate(zip(SCREENS_PAGES, shots), 1):
        name = f"dv_s{i:02d}"
        with Image.open(shot["png"]) as im:
            rgb = im.convert("RGB")
        if rgb.size != SCREENS_SIZE:
            raise SystemExit(f"screens {name}: {rgb.size}, expected {SCREENS_SIZE}")
        entries.append({
            "name": name, "kind": "screenshot", "page": page, "source": source,
            "source_sha256": shot["source_sha256"], "rendered_with": "markdown-it 14.1.0 via npx, minimal owned HTML template",
            "browser_version": browser_version, "repository_revision": revision,
            "rgb_sha256": _rgb_sha256(rgb), "width": SCREENS_SIZE[0], "height": SCREENS_SIZE[1],
        })
        rgb.save(SCREENS_ASSETS / f"{name}.png", optimize=True)
        rgb.save(ppm_dir / f"{name}.ppm")
    _refuse_heldout_overlap(entries)
    SCREENS_MANIFEST.write_text(json.dumps(
        {"set": "screens", "frozen": True,
         "note": "Development screenshots for D01 T07: rounds are designed and decided on these, never on research/heldout.json. "
                 "Later runs verify against these hashes and refuse mismatches without rewriting this file.",
         "images": entries}, indent=2) + "\n", encoding="utf-8")
    verify_screens(ppm_dir)


def heldout(ppm_dir: Path = PPM) -> None:
    ppm_dir.mkdir(parents=True, exist_ok=True)
    cache = DATA / "corpus" / "heldout"
    cache.mkdir(parents=True, exist_ok=True)
    if HELDOUT_MANIFEST.is_file():
        manifest = json.loads(HELDOUT_MANIFEST.read_text(encoding="utf-8"))
        missing = [e for e in manifest["images"] if not (ppm_dir / f"{e['name']}.ppm").is_file()]
        if not missing:
            verify_heldout(ppm_dir)
            return
        # Regenerate only the missing crops, then verify against the frozen manifest.
        by_name = {p["name"]: p for p in HELDOUT_PHOTOS}
        shots = [e for e in missing if e["kind"] == "screenshot"]
        # Frozen assets first: the committed PNGs under
        # research/heldout-assets/ are the exact frozen pixels, so recovery
        # on any host is a file copy, never a re-render. Rendering (with
        # its pinned browser) remains only for authoring a new set.
        frozen_done = []
        for e in shots:
            asset = REPO / "research" / "heldout-assets" / f"{e['name']}.png"
            if asset.is_file():
                with Image.open(asset) as im:
                    im.convert("RGB").save(ppm_dir / f"{e['name']}.ppm")
                frozen_done.append(e["name"])
        shots = [e for e in shots if e["name"] not in frozen_done]
        missing = [e for e in missing if e["name"] not in frozen_done]
        if shots:
            import hashlib as _hl
            _stage = REPO / "build" / "site-source"
            subprocess.run([sys.executable, str(REPO / "scripts" / "build-docs.py"), "--stage", "build/site-source"],
                           cwd=REPO, check=True, capture_output=True, text=True)
            for e in shots:
                _page = e["source_page"].removeprefix("build/site-source/").removesuffix(".md")
                _md = _stage / f"{_page}.md"
                if not _md.is_file() or _hl.sha256(_md.read_bytes()).hexdigest() != e.get("source_sha256"):
                    raise SystemExit(f"heldout screenshot recovery: staged source for {_page} differs from the frozen source hash; refusing to regenerate a changed page")
            rendered = {s["page"]: s for s in _screenshot_pages(shots[0].get("browser_version"))}
            page_of = {e["name"]: e["source_page"].removeprefix("build/site-source/").removesuffix(".md") for e in shots}
            for e in shots:
                shot = rendered[page_of[e["name"]]]
                with Image.open(shot["png"]) as im:
                    im.convert("RGB").save(ppm_dir / f"{e['name']}.ppm")
            missing = [e for e in missing if e["kind"] == "photograph"]
        for e in missing:
            src = by_name[e["name"]]
            raw = cache / (e["name"] + ".jpg")
            data = _download_validated(src["download"], raw)
            with Image.open(raw) as im:
                rgb = im.convert("RGB")
            w, h = rgb.size
            rgb.crop(((w - 1280) // 2, (h - 960) // 2, (w - 1280) // 2 + 1280, (h - 960) // 2 + 960)).save(ppm_dir / f"{e['name']}.ppm")
        verify_heldout(ppm_dir)
        return
    entries = []
    for src in HELDOUT_PHOTOS:
        raw = cache / (src["name"] + ".jpg")
        import hashlib
        data = _download_validated(src["download"], raw)
        with Image.open(raw) as im:
            rgb = im.convert("RGB")
        w, h = rgb.size
        crop = rgb.crop(((w - 1280) // 2, (h - 960) // 2, (w - 1280) // 2 + 1280, (h - 960) // 2 + 960))
        crop.save(ppm_dir / f"{src['name']}.ppm")
        import time as _time
        _time.sleep(3)
        entries.append({
            "name": src["name"], "kind": "photograph", "title": src["title"],
            "source_page": src["page"], "download_url": src["download"],
            "download_sha256": hashlib.sha256(data).hexdigest(),
            "licence": src["licence"], "licence_url": src["licence_url"], "author": src["artist"],
            "source_revision": src["source_revision"],
            "licence_evidence": "Commons API imageinfo/extmetadata and revisions query, 2026-11-09; page revision id in source_revision",
            "licence_verified": "Commons API imageinfo extmetadata at authoring, 2026-11-09",
            "alteration": "original download, center crop to 1280x960",
            "rgb_sha256": _rgb_sha256(crop), "width": 1280, "height": 960,
        })
        print("heldout", src["name"], flush=True)
    for i, shot in enumerate(_screenshot_pages(), 1):
        name = f"ho_s{i:02d}"
        with Image.open(shot["png"]) as im:
            rgb = im.convert("RGB")
        if rgb.size != (1280, 960):
            raise SystemExit(f"heldout screenshot {name}: {rgb.size}, expected (1280, 960)")
        rgb.save(ppm_dir / f"{name}.ppm")
        entries.append({
            "name": name, "kind": "screenshot", "source_page": f"build/site-source/{shot['page']}.md",
            "rendered_with": "markdown-it 14.1.0 via npx, minimal owned HTML template",
            "source_sha256": shot["source_sha256"],
            "browser": shot["browser"], "browser_version": shot["browser"].rsplit("(", 1)[-1].removesuffix(")"), "repository_revision": subprocess.run(
                ["git", "rev-parse", "HEAD"], cwd=REPO, capture_output=True, text=True).stdout.strip(),
            "rgb_sha256": _rgb_sha256(rgb), "width": 1280, "height": 960,
        })
        print("heldout", name, flush=True)
    HELDOUT_MANIFEST.write_text(json.dumps(
        {"set": "heldout", "frozen": True,
         "note": "Never used to choose a parameter. Later runs verify against these hashes and refuse mismatches without rewriting this file.",
         "images": entries}, indent=1) + "\n", encoding="utf-8")
    verify_heldout(ppm_dir)


def main() -> None:
    PPM.mkdir(parents=True, exist_ok=True)
    Image.open(REPO / "measure" / "image.bmp").convert("RGB").save(PPM / "measure.ppm")
    for i in range(1, 25):
        png = DATA / "corpus" / "kodak" / f"kodim{i:02d}.png"
        fetch(f"https://r0k.us/graphics/kodak/kodak/kodim{i:02d}.png", png)
        Image.open(png).convert("RGB").save(PPM / f"kodim{i:02d}.ppm")
    for png in sorted((REPO / "testdata" / "photos").glob("*_crop.png")):
        Image.open(png).convert("RGB").save(PPM / f"tp_{png.stem}.ppm")
    zpath = DATA / "corpus" / "icinfo" / "rgb8bit.zip"
    fetch("http://imagecompression.info/test_images/rgb8bit.zip", zpath)
    with zipfile.ZipFile(zpath) as z:
        for info in z.infolist():
            if info.filename.endswith(".ppm"):
                im = Image.open(io.BytesIO(z.read(info))).convert("RGB")
                w, h = im.size
                cw, ch = min(1280, w), min(960, h)
                x0, y0 = (w - cw) // 2, (h - ch) // 2
                im.crop((x0, y0, x0 + cw, y0 + ch)).save(PPM / f"ic_{Path(info.filename).stem}.ppm")
    from benchmark import synthetic  # noqa: E402
    for name, w, h, rgb in synthetic():
        Image.frombytes("RGB", (w, h), rgb).save(PPM / f"syn_{name}.ppm")

    heldout(PPM)
    screens(PPM)

    update_refs(PPM, DATA / "jxl", DATA / "jxl_refs.json")
    print(f"{len(list(PPM.glob('*.ppm')))} images in {PPM}")


if __name__ == "__main__":
    if "--verify-heldout" in sys.argv:
        verify_heldout()
    elif "--author-screens" in sys.argv:
        PPM.mkdir(parents=True, exist_ok=True)
        author_screens(PPM)
    else:
        main()

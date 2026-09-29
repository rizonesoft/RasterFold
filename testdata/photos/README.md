# Photo test pack

Ten photographs for measuring RasterFold against other lossless codecs on
ordinary camera images, in addition to `measure/image.bmp`. `manifest.json`
lists each file's source, size, any alteration, and the SHA-256 of its RGB
samples (Pillow `convert("RGB").tobytes()`, row-major, top to bottom).

| file | size | source |
|---|---|---|
| kodim01.png | 768×512 | Kodak Lossless True Color Image Suite |
| kodim03.png | 768×512 | Kodak Lossless True Color Image Suite |
| kodim05.png | 768×512 | Kodak Lossless True Color Image Suite |
| kodim13.png | 768×512 | Kodak Lossless True Color Image Suite |
| kodim23.png | 768×512 | Kodak Lossless True Color Image Suite |
| deer_crop.png | 1024×768 | imagecompression.info, `deer.ppm`, cropped |
| fireworks_crop.png | 1024×768 | imagecompression.info, `fireworks.ppm`, cropped |
| flower_foveon_crop.png | 1024×768 | imagecompression.info, `flower_foveon.ppm`, cropped |
| nightshot_iso_1600_crop.png | 1024×768 | imagecompression.info, `nightshot_iso_1600.ppm`, cropped |
| spider_web_crop.png | 1024×768 | imagecompression.info, `spider_web.ppm`, cropped |

## Sources and terms

**Kodak.** Downloaded from <https://r0k.us/graphics/kodak/>, which describes
the images as released by the Eastman Kodak Company for unrestricted usage.
The PNG files here were re-encoded; their pixels are identical to the source.

**imagecompression.info.** From "The New Image Compression Test Set - Jan
2008", <http://imagecompression.info/test_images/rgb8bit.zip>, 8-bit RGB
set. Each file here is an **altered version**: a 1024×768 center crop of the
original photograph, saved as PNG. The crop rectangle is in `manifest.json`.
The pictures are © their respective owners and are redistributed under the
terms in `NOTICE-imagecompression-info.txt`, which is included unmodified as
those terms require. These files may not be sold.

The Kodak images are 1990s film scans. The crops come from digital cameras
(Canon, Nikon, Sigma Foveon), include ISO 1600 sensor noise and a dark night
sky, and cover the textures that decide lossless photo size.

# Status

Aim: not met. `guard/CHASE` is not present, so the stop hook is disarmed.

Final image: `measure/image.bmp` (1672×941). The size to beat is lossless JPEG XL at 1470703 bytes. Decode time and peak memory must match that JPEG XL run or do better. On the latest run RasterFold decoded in 52133 µs with peak working set 20111360, against JPEG XL at 526986 µs and 51814400. The version 1 `.rfi` is 4047356 bytes. The only miss is size.

Next measurement: `python tests/measure_final.py`.

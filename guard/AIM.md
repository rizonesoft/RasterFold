# RasterFold aim

The final image is `measure/image.bmp`. The judge is `python tests/measure_final.py`. Exit 0 is the only time the aim is met. The skill `.grok/skills/rasterfold-aim/SKILL.md` is the worker procedure.

That BMP is one ordinary picture. Improvements must be general lossless methods and must still round-trip other images. Do not detect this file or add a path that only helps it.

After a codec change that passes `python tests/test_correctness.py`, or a measurement that improves the `.rfi` size, commit and push to https://github.com/rizonesoft/RasterFold. Never commit or push `.grok`.

`/rasterfold-guard-start` creates `guard/CHASE` and the durable 10-minute schedule. `/rasterfold-guard-stop` deletes that schedule and writes `guard/STOP`. A cycle already running finishes its current improvement, writes `guard/status.md`, then deletes `guard/lock`, `guard/CHASE`, and `guard/STOP`. It does not start another improvement. `guard/STOP` also makes the stop hook let the turn end.

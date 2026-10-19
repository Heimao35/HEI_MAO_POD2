# Animation regression fixture

`baseline-460.mjpeg` contains two synthetic FFmpeg test-pattern frames generated
for this project, no third-party video. Baseline JPEG, 460x460, YUV420, 24fps.
Generation and independent decoder validation:

```
ffmpeg -f lavfi -i testsrc2=size=460x460:rate=24 -frames:v 2 -c:v mjpeg -pix_fmt yuvj420p -q:v 8 -f mjpeg baseline-460.mjpeg
ffmpeg -i baseline-460.mjpeg -f null -
```

The host suite runs actual import/CRC/persistence/frame iteration on this file.
It does not measure ESP32-P4 decoding speed or verify the physical panel.

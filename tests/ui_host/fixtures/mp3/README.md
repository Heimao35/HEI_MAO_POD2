# MP3 regression fixtures

Synthetic 0.3-second sine waves, generated for this project (no third-party music).
Left/mono: amplitude 0.3 at 997 Hz; right: amplitude 0.23 at 1591 Hz.
FFmpeg/libmp3lame encoded without ID3 or Xing tags; FFmpeg decoded reference is s16le.
48k stereo: CBR 128kbps; 44.1k stereo: VBR q3; 32k mono: CBR64;
24k mono MPEG2: CBR48. The latter exercises the decoder only; board DAC rate policy is unchanged.

Example generation (substitute rates/channel expressions and bitrate as appropriate):

```
ffmpeg -f lavfi -i "aevalsrc=0.3*sin(2*PI*997*t)|0.23*sin(2*PI*1591*t):s=48000:d=0.3" -c:a libmp3lame -b:a 128k -write_xing 0 -id3v2_version 0 -map_metadata -1 48k_stereo_cbr.mp3
ffmpeg -i 48k_stereo_cbr.mp3 -f s16le -acodec pcm_s16le 48k_stereo_cbr.pcm
```

Tests compare frame-split decode and continuous decode against this independent reference.
These clips validate functional decoding, not ESP32-P4 throughput.

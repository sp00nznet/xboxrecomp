# Synthetic WMA fixture

`sine.wma` is a generated 440 Hz tone: 0.25 seconds, 44,100 Hz, stereo,
encoded as WMA v2 at 64 kbit/s. It contains no game or recorded audio.
The synthetic fixture is dedicated to the public domain under CC0-1.0.

Generate it with FFmpeg:

```sh
ffmpeg -f lavfi -i 'sine=frequency=440:duration=0.25:sample_rate=44100' \
  -ac 2 -c:a wmav2 -b:a 64000 -map_metadata -1 sine.wma
```

FFmpeg is only needed to regenerate the fixture, not to run the test.
The test checks successful decoding and PCM metadata, then injects a source
reader error after the first sample to check partial-output cleanup.

# OBS-fun

A growing collection of not-entirely-serious OBS Studio video/audio filter
plugins. Each filter lives in its own subdirectory with its own build,
tests, and CI.

## Filters

* [`JPEGlitch/`](JPEGlitch/) -- applies the classic "glitch art" look live:
  encodes the frame as JPEG, flips bits in the compressed bitstream, decodes
  the (now corrupted) result. Ported from the original Qt desktop app
  [607011/qlitch](https://github.com/607011/qlitch) (2013).

See [`IDEAS.md`](IDEAS.md) for a loose list of candidates for future filters.

## License

Each filter subdirectory carries its own license file; see the respective
subdirectory for details.

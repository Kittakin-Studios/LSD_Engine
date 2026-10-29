# Clean-room rules

LSD_Engine takes architectural *ideas* from public talks, blogs and books and implements
them from scratch. It must never contain code or data derived from proprietary engines.

## Allowed

- Public blog posts, conference talks, papers and books (read, learn, then write our own code).
- Open-source code under a permissive licence (MIT, BSD, zlib, Apache-2.0, public domain),
  added under `third_party/` with its licence file kept. LSD_Engine is proprietary, so avoid
  GPL code entirely, and use LGPL code only as a separately linked library.
- Our own names, file formats and APIs.

## Not allowed

- Autodesk Stingray / Bitsquid binaries, decompiled or disassembled output, leaked source,
  or licensee-only documentation.
- Copying code shown on screen in talks or videos, including walkthroughs of proprietary
  engines. Take notes on the *idea*, close the video, then write the code yourself.
- Reproducing another engine's file formats or API names byte-for-byte.

## Reference material (ideas only)

- Bitsquid development blog — https://bitsquid.blogspot.com/
- Niklas Gray's (Frykholm's) YouTube channel — https://www.youtube.com/@niklasfrykholm/videos
- Data-Oriented Design, Richard Fabian — https://www.dataorienteddesign.com/dodbook/
- Game Engine Architecture, Jason Gregory — https://www.gameenginebook.com/

R1 for logue
============


A Korg M1 player unit for the KORG drumlogue and the microKORG2, built on the
PicoFaceM1 engine from
[PicoVintageSynthCollection](https://github.com/Michi71/PicoVintageSynthCollection)


## ROMs

Put these in the host's userfs, beside the units. The `m1-` prefix keeps them
apart from other units' files:

```
/Programs/
  m1-ic23.bin          the two 64 KB halves of the original M1 OS (Rev 19)
  m1-ic32.bin
  m1-ic30.bin          64 KB coefficient ROM (IC20 or IC26 is the same chip)
  m1-ic36.bin .. m1-ic43.bin   PCM, 512 KB each (IC36-39 low, IC40-43 high)
  m1-preload.syx       optional: an M1 ALL DATA dump (factory programs)
```

The loader finds which OS half is even by content and rejects the Plus+1 OS,
which is laid out differently. Without `m1-preload.syx` the PROGRAM parameter
selects a multisound and the unit builds a plain single-oscillator program for
it; with the preload it selects the factory programs. The M1 effect section is
bypassed in both cases.

On the microKORG2 the userfs is the `Programs` folder of the drive.

`tools/prepare-roms.sh <rom-dir> <dest-dir> [<all-data.syx>]` stages a ROM tree
under these names.


## How to build

Both builds run in the logue SDK container and need a logue-sdk checkout:

```
LOGUE_SDK_PATH=~/Projects/logue/logue-sdk ./build-drumlogue.sh
LOGUE_SDK_PATH=~/Projects/logue/logue-sdk ./build-microkorg2.sh
```

The results are `bin/R1.drmlgunit` for the drumlogue and `bin/R1.mk2unit` for
the microKORG2. Install the first at `Units/Synths/R1.drmlgunit` on the
drumlogue; install the second in a `Units/Oscs/SlotN` folder on the
microKORG2 and select `R1` as the oscillator.


## Host check

```
M1_ROM_DIR=<rom-dir> tools/hostcheck-drumlogue.sh
M1_ROM_DIR=<rom-dir> tools/hostcheck-microkorg2.sh
```

Both compile the unit, `header.c`, the engine sources and the `-D`'s from
`config.mk` natively, stage the ROMs from `M1_ROM_DIR`, and drive the unit
through the host's callbacks.

The drumlogue check covers the parameters, that a note sounds and ends, two
keys together, the step gate, PROGRAM and both ends of LEVEL, and reports the
render time against the 1.33 ms block.

The microKORG2 check drives the oscillator context and compares the unit's
stereo pair, block by block, with one reference engine given the same notes:
a note, a chord, eight voices, voice stealing, pitch steps and bends, a
repeated key, a tap inside one block, PROGRAM 1, and the release to silence.


## Parameters

The drumlogue unit:

| # | Name | Range | Function |
| - | ---- | ----- | -------- |
| 0 | PROGRAM | 0-99 | factory program, or multisound without a preload |
| 1 | LEVEL | 0-100 % | output level |
| 2 | NOTE | 0-127 | the note a gate plays |

The microKORG2 oscillator has one parameter, PROGRAM 0-99; the keyboard
supplies the notes, velocity and pitch.


## License
MIT for this project's own code. The files that bind the engine are
GPL-3.0-or-later because the engine is GPL-3.0, and so are the built units:
`m1_drumlogue/unit.cc`, `m1_drumlogue/m1_synth.h`, `m1_microkorg2/unit.cc`,
`m1_microkorg2/osc.h`, `common/m1_rom.h`, `common/m1_resampler.h`,
`common/pvsc_m1_engine.cc` and `common/pvsc_m1_fx.cc`. See `LICENSE`,
`LICENSE-GPL-3.0` and the engine's own licence.


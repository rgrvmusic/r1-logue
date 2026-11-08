Install
=======

## Unit

| File | Host | Where |
| ---- | ---- | ----- |
| `bin/R1.drmlgunit` | drumlogue | `Units/Synths/R1.drmlgunit` |
| `bin/R1.mk2unit` | microKORG2 | `Units/Oscs/SlotN/R1.mk2unit`, then select `R1` as the oscillator |

## ROMs

Required, in the `Programs` folder of the host's drive (userfs). No ROM file is
shipped with the unit.

| File | Size | Contents |
| ---- | ---- | -------- |
| `m1-ic23.bin` | 64 KB | M1 OS, Rev 19, one half |
| `m1-ic32.bin` | 64 KB | M1 OS, Rev 19, other half |
| `m1-ic30.bin` | 64 KB | coefficient ROM; `ic20.bin` or `ic26.bin` is accepted in its place |
| `m1-ic36.bin` .. `m1-ic43.bin` | 512 KB each | PCM, IC36-39 low bytes, IC40-43 high bytes |

Optional:

| File | Size | Contents |
| ---- | ---- | -------- |
| `m1-preload.syx` | M1 ALL DATA dump | factory programs |

Without the preload the PROGRAM parameter selects one of the 100 multisounds.

Stage a ROM tree under these names with:

```
tools/prepare-roms.sh <rom-dir> <programs-dir> [<all-data.syx>]
```

# xl710_unlock

Clears the "Enable Module Qualification" bit in the NVM of Intel X710/XL710
NICs, so the card stops refusing SFP+/QSFP+ modules Intel hasn't blessed.

Derived from [terpstra/xl710-unlocker](https://github.com/terpstra/xl710-unlocker)
and [bibigon812/xl710-unlocker](https://github.com/bibigon812/xl710-unlocker),
with the structure-location problem actually solved rather than hardcoded.

## The bit

Every known card and firmware agrees on one thing: bit 11 of *PHY Capabilities
Misc0*, at word `+0x08` of each per-port PHY capabilities struct. Clear it on
every port and the qualification check stops.

Nothing else is stable. The struct's base offset and its size both move with
every firmware revision:

| card / firmware | base | size word | stride | Misc0 |
|---|---|---|---|---|
| X710, fw 5.x | `0x6870` | `0x0b` | `0x0c` | `0x2b0c` |
| X710, fw 6.x | `0x68f0` | `0x0c` | `0x0d` | `0x630c` |
| X710, fw 8.13 | `0x6940` | `0x0d` | `0x0e` | `0x6b0c` |
| XL710-QDA2 (`0x1583`) | `0x67fa` | `0x0b` | `0x0c` | `0x0b10` |
| XL710-QDA2, EETRACK `0x8001037b` | `0x69c4` | `0x0d` | `0x0e` | `0x0310` |

Both upstreams hardcode the stride at `0xc`, so on anything newer than fw 5.x
they patch words belonging to the *next* struct. That is how people end up with
a card that reports success and then won't POST.

## How this tool finds the structs

Not by arithmetic. The Shadow RAM pointer chain (`word 0x48` → EMP SR module →
`+0x19` → PHY capabilities) is only treated as a *hint*; Intel's own HOWTO warns
the image pointer may be corrupted, and the QDA2 reporter in upstream issue #6
couldn't resolve it at all.

Instead the section is **recognised** by its signature: word 0 of a struct is its
payload size, structs sit `size + 1` apart, and that size word therefore repeats
at exactly that stride. That holds on every dump in the table above. The hint is
validated against the signature; if it fails, the whole Shadow RAM is scanned for
something that matches. Nothing is written unless the signature checks out.

This also rejects the classic false positive. Firmware 8.x has the constant
`0x000b` sitting at struct offset `+07` — exactly what the *size word* was on
firmware 5.x. Everyone greps for `000b`, lands on `base+7`, reads `+0x08` from
there, finds bit 11 clear, and concludes the card is already unlocked. Deriving
the stride *from* the size word kills this: from the false base the implied
stride predicts a repeat that isn't there.

Other fixes over upstream:

| | upstream | here |
|---|---|---|
| stride | hardcoded `0xc` | `size + 1`, read from the image |
| base | pointer chain, unvalidated | recognised by signature, pointer only a hint |
| device ID | hardcoded `0x1572` | sysfs; vendor and `i40e` driver verified |
| patch value | writes the value left over from the last read into every struct | each struct read-modify-written from its own value |
| bit flip | `misc ^ 0x0800`, which re-locks an already-unlocked struct | explicit set/clear, idempotent |
| verification | none | every write read back and compared |
| undo | none | `-l` restores the check |

## Build

```shell
make
make test        # selftest against the real card dumps above; no hardware
sudo make install
```

## Usage

Report only — reads nothing but the NVM, writes nothing:

```shell
# ./xl710_unlock -n enp1s0f0
enp1s0f0: device 0x1583
EMP SR at 0x6874, pointer chain suggests 0x69c4
pointer chain validated
PHY capabilities at 0x69c4: 4 struct(s), size 0x0d, stride 0x0e
  port 0  struct 0x69c4  Misc0 @ 0x69cc = 0x0b10  locked
  port 1  struct 0x69d2  Misc0 @ 0x69da = 0x0b10  locked
  port 2  struct 0x69e0  Misc0 @ 0x69e8 = 0x0b10  locked
  port 3  struct 0x69ee  Misc0 @ 0x69f6 = 0x0b10  locked

4 of 4 struct(s) locked. Pass -u to unlock.
```

Unlock with `-u`, undo with `-l`. Then **power-cycle at the wall** — the EMP only
re-reads this section at power-on, so a warm reboot or driver reload won't pick
it up.

```
-n <iface>   interface to operate on (required unless -f)
-f <dump>    analyse a saved image instead of a card, read-only
-c <image>   also diff against a reference NVM image (an Intel .bin)
-u           unlock: accept unsupported modules
-l           lock: restore Intel's qualified-module check
-y           don't ask for confirmation
-b <base>    force the PHY capabilities base word offset
-d <word>    dump NVM words starting here, then exit
-N <count>   words to dump (default 32)
-i <devid>   override the PCI device ID from sysfs
-t           run the selftest, then exit
```

## Diagnosing without touching the card

Save an image and analyse it offline — same recogniser, no write path at all:

```shell
sudo ethtool -e enp1s0f0 raw on > nvm.bin
./xl710_unlock -f nvm.bin
./xl710_unlock -f nvm.bin -d 0x69c4 -N 64
```

A genuine PHY capabilities struct on an XL710 looks like this, with the port
index in the high byte of the last word — that's the strongest confirmation you
have the right place:

```
  69c4 + 00 => 000d      <- size
  69c4 + 01 => 0023
  ...
  69c4 + 08 => 0b10      <- Misc0, bit 11 set = locked (0x0310 = unlocked)
  ...
  69c4 + 0b => 0002      <- port 0; next structs show 0102, 0202, 0302
```

## If it says "unlocked" but the card still rejects modules

Check these in order before touching anything:

1. **Power-cycle at the wall.** Not `reboot`, not a driver reload. Pull the cord
   or switch the PSU off. This is by far the most common cause.
2. **Confirm the message.** `dmesg | grep -i i40e`. Only
   *"unsupported SFP module type was detected"* is the qualification check.
   Anything else is a different problem.
3. **Confirm the struct.** `-d <base> -N 64` and look for the `0002/0102/0202/0302`
   port indices. If they're absent, the base is wrong.
4. **Verify the NVM checksum.** A previous tool that wrote bit 11 without
   refreshing the checksum leaves an image the EMP may reject wholesale. This
   tool always refreshes it after a write.
5. **Diff against the image Intel ships for your card.** `-c <image>` against
   the `.bin` files in the NVM update package. If your EETRACK is the `EEPID`
   of a *locked* build but bit 11 is clear, the card has been hand-patched into
   a state no firmware ships — see the firmware 9.x section below, where
   clearing bit 11 is not sufficient.

## Firmware 9.x: bit 11 alone is not enough

On firmware 9.x, "open optics" is a **separate NVM build**, not a bit you flip.
Intel's own update package proves it. For the same firmware version 9.57 it
ships three XL710-QDA2 images:

| image | EEPID | Misc0 | |
|---|---|---|---|
| `XL710QDA2_9p57_CFGID4p5_OEMGEN.bin` | `8001037B` | `0x0b10` | locked |
| `XL710QDA2_9p57_CFGID4p5_OEMGEN_OO.bin` | `80010385` | `0x0310` | unlocked |
| `XL710QDA2_9p57_CFGID4p5_K15190.bin` | `8001035A` | `0x0310` | unlocked |

So bit 11 is still meaningful on 9.x — but it is not the only difference. The
two unlocked builds differ from the locked one by ~1870 words *outside* the
64 KB Shadow RAM, while differing from **each other** by only 18. That shared
content is the open-optics part, and no amount of patching the Shadow RAM
produces it. Among other things the unlocked builds carry extra port-config
entries (`1x40 LOM`, `2x40 LOM`, `4x10LOM`, `2x2x10LOM`) that the locked build
lacks; the rest of the config table is identical, so nothing is lost by moving
to them.

There is also a header word that tracks the firmware era. Word `+0x01` of each
PHY capabilities struct reads `0x0223` in every 9.x image and `0x0222` on X710
fw 8.13, but `0x0023`/`0x0022` on 4.42-era and fw 6.x images.

### A card patched by hand ends up in a state no build ships

One XL710-QDA2 (FW 9.57) showed exactly this. `-c` against Intel's images:

```
EETRACK 0x8001037b                       <- the LOCKED OEMGEN build
  port 0..3  Misc0 = 0x0310              <- but bit 11 clear, like OO
vs XL710QDA2_9p57_CFGID4p5_OEMGEN_OO.bin:
  EETRACK   card 0x8001037b   image 0x80010385
  port 0 +01: card 0023  image 0223      <- pre-8.x header word
  ...
```

The card runs the locked build, with bit 11 cleared and a stale `+0x01`. It
matches no shipped image, and the firmware refuses third-party optics anyway:
`Rx/Tx is disabled ... unsupported SFP module type was detected`, with
`ethtool` reporting `Supported link modes: Not reported` (i.e.
`i40e_aq_get_phy_abilities()` returned nothing, so the EMP is holding the PHY
down rather than the driver declining the module).

Its NVM checksum validated and `phy_type` already permitted `40GBASE_CR4`, so
the module was not being refused on type or speed. Clearing bit 11 — all this
tool or any other can do — was already done and had not helped.

**The fix in that situation is to flash Intel's open-optics build, not to patch
bits.** `nvmupdate64e` selects an image by matching the card's current EETRACK
against a block's `REPLACES` list, and a card already at `8001037B` is the
locked build's own `EEPID`, so the tool considers it up to date and will not
cross over to the OO lineage. The two `nvmupdate.cfg` blocks are otherwise
identical — same `VENDOR: 8086`, `DEVICE: 1583`, same `EEPROM MAP`, same OROM —
so routing the card to the OO block lets Intel's own tool do the write, with
its own checksums and MAC preservation.

Two edits are needed, not one — adding the EETRACK to `REPLACES` still leaves
the locked block declaring that same EETRACK as its `EEPID`, which is what
makes `nvmupdate` answer "up to date". `nvmupdate-cfg-patch.py` makes both and
verifies the result:

```shell
./nvmupdate-cfg-patch.py /path/to/nvmupdate.cfg \
    --eetrack 8001037B --device 1583 \
    --image XL710QDA2_9p57_CFGID4p5_OEMGEN_OO.bin
```

```
before:
  already-current  XL710QDA2_9p57_CFGID4p5_OEMGEN.bin (EEPID 8001037B)
edit 1: added 8001037B to REPLACES of XL710QDA2_9p57_CFGID4p5_OEMGEN_OO.bin
edit 2: commented out XL710QDA2_9p57_CFGID4p5_OEMGEN.bin
after:
  updates          XL710QDA2_9p57_CFGID4p5_OEMGEN_OO.bin (EEPID 80010385)
```

It keeps the file CRLF, keeps `BEGIN`/`END DEVICE` balanced, and refuses to
write unless exactly one block would update the card and none still claims that
EETRACK as its own `EEPID` — all three are easy to get wrong by hand in a file
that drives a firmware write. `-n` dry-runs it; the original is saved as
`.orig`. Then:

```shell
sudo ./nvmupdate64e            # from the directory with the patched cfg
# cold power cycle, then confirm:
sudo ./xl710_unlock -n <iface> -c XL710QDA2_9p57_CFGID4p5_OEMGEN_OO.bin
#   want: EETRACK 0x80010385 and "structs are identical"
```

Back the card up first (see `nvmupdate64e -h`) and keep the locked image so you
can go back. Note this retargets only the given EETRACK: a QDA2 at some *other*
version that the locked block would have updated will now report no update,
since those ids stay in the disabled block.

## Notes

- The NVM is shared by all ports; patch one interface, not each in turn.
- Four structs are present even on a two-cage QDA2 — one per internal PHY lane.
  All four are patched.
- `nvmupdate64e -rd` restores the lock bits. A normal NVM update preserves them.
- This writes to flash. It is reversible with `-l`, the layout is validated
  before any write and every write is read back, but a half-written NVM may need
  recovery with Intel's `nvmupdate64e`. Original values are printed before
  anything changes — keep them.

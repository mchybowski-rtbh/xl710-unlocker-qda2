#!/usr/bin/env python3
"""Route a card at a given EETRACK to a different NVM image in nvmupdate.cfg.

Why this exists: on firmware 9.x, "open optics" is a separate NVM build rather
than a bit you flip (see README). nvmupdate picks an image by matching the
card's current EETRACK against a block's REPLACES list, so a card already
sitting at the locked build's own EEPID is reported as up to date and never
crosses to the open-optics lineage. Two edits fix that:

  1. add the card's EETRACK to the target image's REPLACES list
  2. comment out the block whose EEPID equals that EETRACK, which is what
     makes nvmupdate conclude "up to date" instead of updating

Doing this by hand is easy to get wrong in ways that matter, because the file
drives a firmware write: the config is CRLF and must stay CRLF, BEGIN/END
DEVICE must stay balanced, and afterwards exactly one block should claim the
card. This checks all three and refuses to write otherwise.

  ./nvmupdate-cfg-patch.py nvmupdate.cfg \\
      --eetrack 8001037B --device 1583 \\
      --image XL710QDA2_9p57_CFGID4p5_OEMGEN_OO.bin

Back up the card first (see `nvmupdate64e -h`) and keep the locked image so
you can go back.
"""

import argparse
import shutil
import sys

CRLF = b"\r\n"


def blocks(lines):
    """Yield (start, end) index pairs of each uncommented BEGIN/END DEVICE."""
    i = 0
    while i < len(lines):
        if lines[i].strip() == b"BEGIN DEVICE":
            j = i
            while j < len(lines) and lines[j].strip() != b"END DEVICE":
                j += 1
            if j < len(lines):
                yield i, j
                i = j
        i += 1


def field(blk, name):
    for line in blk:
        if line.startswith(name + b":"):
            return line.split(b":", 1)[1].strip()
    return None


def claims(lines, device, eetrack):
    """Blocks for `device` that either update this card or declare it current."""
    out = []
    for s, e in blocks(lines):
        blk = lines[s:e + 1]
        if field(blk, b"DEVICE") != device:
            continue
        img = field(blk, b"NVM IMAGE") or b"?"
        eep = field(blk, b"EEPID")
        repl = field(blk, b"REPLACES")
        if repl and eetrack in repl.split():
            out.append(("updates", img, eep, s, e))
        if eep == eetrack:
            out.append(("already-current", img, eep, s, e))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cfg")
    ap.add_argument("--eetrack", required=True,
                    help="card's current EETRACK, e.g. 8001037B")
    ap.add_argument("--device", required=True, help="PCI device id, e.g. 1583")
    ap.add_argument("--image", required=True,
                    help="target NVM IMAGE basename to route the card to")
    ap.add_argument("-n", "--dry-run", action="store_true")
    a = ap.parse_args()

    eetrack = a.eetrack.strip().upper().encode()
    device = a.device.strip().lower().lstrip("0x").encode()
    image = a.image.strip().encode()

    raw = open(a.cfg, "rb").read()
    if raw.count(b"\n") != raw.count(CRLF):
        sys.exit("error: %s is not pure CRLF; refusing to touch it" % a.cfg)
    lines = raw.split(CRLF)
    begins = sum(1 for l in lines if l.strip() == b"BEGIN DEVICE")

    print("before:")
    before = claims(lines, device, eetrack)
    for kind, img, eep, _, _ in before:
        print("  %-16s %s (EEPID %s)" % (kind, img.decode(), eep.decode()))
    if not before:
        print("  nothing claims %s" % eetrack.decode())

    # locate the target block by its image name
    target = None
    for s, e in blocks(lines):
        if field(lines[s:e + 1], b"NVM IMAGE") == image:
            target = (s, e)
            break
    if not target:
        sys.exit("error: no block with NVM IMAGE: %s" % a.image)
    if field(lines[target[0]:target[1] + 1], b"DEVICE") != device:
        sys.exit("error: %s is not a DEVICE %s block" % (a.image, a.device))

    # edit 1: target image claims this EETRACK
    for i in range(*target):
        if lines[i].startswith(b"REPLACES:"):
            if eetrack in lines[i].split():
                print("edit 1: %s already in REPLACES, leaving it"
                      % eetrack.decode())
            else:
                lines[i] = lines[i].rstrip() + b" " + eetrack
                print("edit 1: added %s to REPLACES of %s"
                      % (eetrack.decode(), a.image))
            break
    else:
        sys.exit("error: target block has no REPLACES line")

    # edit 2: disable any block declaring this EETRACK as its own EEPID
    disabled = 0
    for kind, img, _, s, e in reversed(claims(lines, device, eetrack)):
        if kind != "already-current":
            continue
        lines[s:e + 1] = [b";" + l for l in lines[s:e + 1]]
        lines[s:s] = [
            b";--- disabled: this block's EEPID is " + eetrack + b", which is what",
            b";    makes nvmupdate report the card as up to date. Restore from",
            b";    the .orig file to flash this image again.",
        ]
        print("edit 2: commented out %s" % img.decode())
        disabled += 1
    if not disabled:
        print("edit 2: nothing to disable")

    # verify
    after = claims(lines, device, eetrack)
    updates = [x for x in after if x[0] == "updates"]
    current = [x for x in after if x[0] == "already-current"]
    print("after:")
    for kind, img, eep, _, _ in after:
        print("  %-16s %s (EEPID %s)" % (kind, img.decode(), eep.decode()))

    ok = True
    if len(updates) != 1:
        print("FAIL: %d blocks would update this card, want exactly 1"
              % len(updates))
        ok = False
    if current:
        print("FAIL: a block still declares %s as its EEPID"
              % eetrack.decode())
        ok = False
    if sum(1 for l in lines if l.strip() == b"BEGIN DEVICE") != begins - disabled:
        print("FAIL: BEGIN DEVICE count changed unexpectedly")
        ok = False
    if not ok:
        sys.exit("refusing to write")

    if a.dry_run:
        print("dry run, not writing")
        return
    shutil.copy(a.cfg, a.cfg + ".orig")
    open(a.cfg, "wb").write(CRLF.join(lines))
    print("wrote %s (original saved as %s.orig)" % (a.cfg, a.cfg))


if __name__ == "__main__":
    main()

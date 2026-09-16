# xl710_unlock

Clears the "qualified module only" bit in the NVM of Intel X710/XL710 NICs, so
the card will bring up links on SFP+/QSFP+ modules Intel hasn't blessed.

Based on [bibigon812/xl710-unlocker](https://github.com/bibigon812/xl710-unlocker),
fixed to work correctly on the **XL710-QDA2** (PCI device `0x1583`).

## Why the original doesn't work on a QDA2

`i40e` overloads the `ETHTOOL_GEEPROM`/`ETHTOOL_SEEPROM` ioctls into an NVM
update channel, reinterpreting `struct ethtool_eeprom` as
`struct i40e_nvm_access`: `magic` becomes `config`, carrying
`(device_id << 16) | (transaction << 8) | module`.

The driver's read path only requires `magic` to be non-zero and different from
the normal ethtool magic, so a **wrong device ID still reads fine**. The write
path checks `(magic >> 16) != hw->device_id` and returns `-EINVAL`. Upstream
hardcodes `0x1572` (X710-DA2), so on a QDA2 it prints a completely plausible
report and then fails at the first write.

Fixed here, along with three other bugs:

| | upstream | here |
|---|---|---|
| device ID | hardcoded `0x1572` | read from sysfs, vendor + driver verified |
| patch value | writes `misc0`, the value left over from the *last* read, into all four structs | each struct is read-modify-written from its own value |
| bit flip | `misc ^ 0x0800`, which *re-locks* an already-unlocked struct | explicit set/clear, idempotent |
| empty structs | written unconditionally | `0x0000`/`0xffff` structs skipped |
| verification | none | every write is read back and compared; layout is sanity-checked before any write |

## Build

```shell
make
make test      # offset/bit selftest, no hardware needed
sudo make install
```

## Usage

Report the current state (reads only, safe):

```shell
# ./xl710_unlock -n enp1s0f0
enp1s0f0: device 0x1583
EMP SR offset:        0x67a8
PHY caps offset:      0x68f6
PHY struct size:      0x000c words
  struct 0 @ 0x68fe  MISC 0x6b0c  locked
  struct 1 @ 0x690b  MISC 0x6b0c  locked
  struct 2 @ 0x6918  MISC 0x6b0c  locked
  struct 3 @ 0x6925  MISC 0x6b0c  locked

4 of 4 struct(s) locked. Pass -u to unlock.
```

Unlock:

```shell
# ./xl710_unlock -n enp1s0f0 -u
...
About to unlock 4 struct(s) in the NVM of enp1s0f0.
To undo, run: xl710_unlock -n enp1s0f0 -l
Continue? [y/N]: y
  struct 0 @ 0x68fe: 0x6b0c -> 0x630c
  struct 1 @ 0x690b: 0x6b0c -> 0x630c
  struct 2 @ 0x6918: 0x6b0c -> 0x630c
  struct 3 @ 0x6925: 0x6b0c -> 0x630c

4 struct(s) patched, NVM checksum updated.
Power-cycle the machine (a warm reboot is not enough) for the change to take effect.
```

`-l` restores Intel's check. All options:

```
-n <iface>    interface to operate on (required)
-u            unlock: accept unsupported modules
-l            lock: restore Intel's qualified-module check
-y            don't ask for confirmation
-i <devid>    override the PCI device ID from sysfs
-c <structs>  PHY capability structs to patch (default 4)
-t            run the offset/bit selftest and exit
```

## Notes

- The NVM is shared by both ports of a QDA2 — patch one interface, not both.
  Run it again on the second interface and it will report "already unlocked".
- A **cold power cycle** is required. The EMP only re-reads the PHY
  capabilities section at power-on; `reboot` or a driver reload won't pick it
  up.
- Four PHY capability structs are patched by default because the XL710 NVM
  carries one per internal PHY lane regardless of how many cages the board
  has. Structs that read back as empty are skipped. Use `-c` if your image
  differs.
- This writes to the card's flash. It is reversible with `-l` and every write
  is verified, but a NIC whose NVM is half-written is a NIC you may have to
  recover with Intel's `nvmupdate64e`. The original MISC values are printed
  before anything is touched — keep them.

## Where the offsets come from

```
Shadow RAM word 0x48            -> EMP SR settings module pointer
EMP module + word 0x19          -> PHY capabilities section (relative pointer)
PHY caps word 0                 -> size of one PHY struct, in words
PHY caps + 0x08 + (size+1)*n    -> MISC word of struct n
MISC bit 11 (0x0800)            -> 1 = qualified modules only
```

Each struct is preceded by its own size word, which is where the `size + 1`
stride comes from.

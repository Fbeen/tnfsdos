# TNFSDRV

A DOS TSR that presents a virtual drive **T:** using the MS-DOS network
redirector API (INT 2Fh / AH=11h).  Intended as the foundation for a
TNFS-over-TCP/IP virtual drive.  Currently serves a hard-coded fake
directory listing to prove the redirector plumbing works end-to-end.

---

## Current status (June 2026 milestone)

Working on MS-DOS 6.20, `CONFIG.SYS` with `LASTDRIVE=Z`:

| Command | Result |
|---|---|
| `TSR.EXE` | Loads, marks T: as a network drive, hooks INT 2Fh |
| `T:` | Accepted as a valid drive letter |
| `CD \` | Changes to T:\ root (INT 2Fh AL=05h handled) |
| `DIR T:\` | Lists `GAMES <DIR>` and `README.TXT  123 bytes` |

Not yet implemented:

- Actual TNFS network I/O (UDP to a TNFS server)
- File open / read / write
- Subdirectory traversal beyond the fake root listing

---

## Build

Requires **Open Watcom C/C++ 2.0** (16-bit DOS target, `wcc` + `wasm` + `wlink` in `PATH`).

```
make
```

Produces in `build/`:

| File | Purpose |
|---|---|
| `TSR.EXE` | Resident driver — load once, stays in memory |
| `SHOWBUF.EXE` | Dumps the in-memory ring buffer to `D:\TNFS\DEBUG.TXT` |
| `FINDTEST.EXE` | Legacy INT 21h AH=4Eh/4Fh test for the C:\TNFS path-hook |

---

## Deploying to DOS machine

Copy `ftp.sh.example` to `ftp.sh`, fill in your machine's address and
credentials, then run `./ftp.sh`.  It builds and FTPs `TSR.EXE` and
`SHOWBUF.EXE` in one step.

`ftp.sh` is in `.gitignore` and will never be committed.

---

## Testing

```
TSR.EXE                   ; load driver; it prints the ring-buffer address
T:                        ; switch to virtual drive T:
CD \                      ; change to root
DIR T:\                   ; list directory
SHOWBUF SSSS:OOOO         ; dump log (address printed by TSR at load time)
```

Expected `DIR T:\` output:
```
 Volume in station T heeft geen naam

 Directory van T:\

GAMES        <DIR>
README   TXT           123
         1 bestand(en)       123 bytes
```

---

## Key implementation discoveries

### CDS flags must be `0xC000`, not `0x8000`

The CDS (Current Directory Structure) entry for T: must have **both**
`CDSFLAG_NET` (0x8000) **and** `CDSFLAG_PHY` (0x4000) set.  MS-DOS 6.x
silently ignores an entry that has only the NET bit.

> *"MS-DOS compat: flagging newly mapped drive so MS-DOS doesn't ignore it"*
> — EtherDFS source

### Resident size must be computed dynamically

The BSS segment contains a 4096-byte ring buffer.  A hardcoded
`RESIDENT_PARAGRAPHS` that is even slightly too small will cause DOS memory
corruption when a subsequent program is loaded.  We compute the correct value
at runtime from `_psp` and Watcom's `_STACKTOP` (initial SP = end of DGROUP).

### FINDFIRST (AL=1Bh) uses `SDA->curr_dta`, not ES:DI

For FINDFIRST, the right approach (following EtherDFS) is:

1. Obtain the SDA pointer at TSR init time via `INT 21h AX=5D06h`.
2. Read `sda->srch_attr` (at SDA+0x24D) to check for volume-label searches.
3. Fill `sda->found_file` (at SDA+0x1B3) with the result.
4. Read `sda->curr_dta` (far pointer at SDA+0x0C) and initialise the SDB
   fields in that DTA buffer (drive letter, search template, `dir_entry`
   sequence counter, etc.).
5. Copy `sda->found_file` to `DTA+0x15`.

ES:DI at the time of the INT 2Fh call points into the SDA but is **not** the
correct place to write the result for FINDFIRST.

### FINDNEXT (AL=1Ch) uses ES:DI as the DTA directly

For FINDNEXT, ES:DI IS the DTA that was set up by our FINDFIRST handler.
Read `DTA[13]` (`sdb.dir_entry`) to decide what entry to return next.  We
use a simple sequence: 1 → README.TXT, ≥2 → EOF.

### SDA offsets (DOS 4+, from EtherDFS `DOSSTRUC.H`)

```
+0x00C  curr_dta     (far pointer, 4 bytes)
+0x1B3  found_file   (32 bytes, foundfilestruct)
+0x22B  fcb_fn1[11]  (search template in FCB format)
+0x24D  srch_attr    (search attribute byte)
```

### CDS entry size and drive index

```c
#define CDS_ENTRY_SIZE  88   /* bytes per CDS entry, DOS 4–6 */
#define DRIVE_T_IDX     19   /* T: = 0-based; A=0, B=1, ..., T=19 */
```

`CONFIG.SYS` must contain `LASTDRIVE=T` (or higher) for CDS index 19 to exist.

---

## Source layout

```
src/
  tsr.c                   Main TSR source
                          ├─ Ring buffer (ISR-safe, no DOS calls)
                          ├─ Logging helpers
                          ├─ LEGACY: INT 21h C:\TNFS path-hook
                          ├─ INT 2Fh redirector handlers
                          │    do_findfirst(), do_findnext()
                          └─ TSR init: CDS setup, SDA pointer, stay-resident
  handler.asm             INT 21h + INT 2Fh interrupt handlers (WASM)
                          ├─ LEGACY: INT 21h AH=4Eh/4Fh/43h path-hook
                          └─ INT 2Fh AH=11h redirector dispatch
  showbuf.c               Companion tool: dumps ring buffer to DEBUG.TXT
  findtest.c              Test tool: exercises legacy INT 21h AH=4Eh/4Fh hook
```

---

## Credits / references

- **[EtherDFS](http://etherdfs.sourceforge.net)** by Mateusz Viste — primary
  technical reference for the MS-DOS redirector API, CDS layout (`DOSSTRUC.H`),
  and SDA structure.  Several field offsets and the `CDSFLAG_NET|CDSFLAG_PHY`
  discovery came directly from reading the EtherDFS source.
- **[RBIL](http://www.ctyme.com/rbrown.htm)** (Ralf Brown's Interrupt List) —
  interrupt reference for INT 21h, INT 2Fh, and DOS internal structures.
- **Open Watcom 2.0** — 16-bit DOS compiler and assembler.

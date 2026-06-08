# TNFSDRV

DOS TSR dat een virtuele drive **N:** aanbiedt via het MS-DOS INT 2Fh network redirector
protocol.  Transport: Ethernet/ARP/IPv4/UDP via een Clarkson packet driver.
Protocol: TNFS (FujiNet-compatible) over UDP/16384.

---

## Actieve architectuur

```
DOS DIR/programma
    ↓ INT 2Fh AH=11h
handler.asm          — interrupt dispatch, stack switch, DS=DGROUP restore
redirector.c         — subfuncties: FINDFIRST, FINDNEXT, GETATTR, OPEN, READ, ...
fs_tnfsmin.c         — FS backend: opendir/readdir/closedir cache in DGROUP
tnfs.c               — TNFS protocol (mount, opendir, readdir, closedir)
netw_pd.c            — Ethernet/ARP/IPv4/UDP, Clarkson packet driver
netw_pd_rcv.asm      — FAR receiver callback (pd_receiver_)
    ↑ INT 60h (3Com 3c509 packet driver)
    ↑ UDP/Ethernet → TNFS server (192.168.178.10:16384)
```

**Debuginfrastructuur:**
- `ringbuf.c` / `ringbuf.h` — ISR-safe ring buffer, post-TSR logging
- `src/dumpbuf.c` — standalone tool: leest ring buffer uit geheugen naar scherm

---

## Bewezen werkend (juni 2026)

| Component | Status |
|---|---|
| N: installeren (CDS + INT 2Fh) | OK |
| Packet driver access_type (INT 60h) | OK |
| Receiver callback (pd_receiver_) | OK |
| ARP request/reply | OK |
| UDP TX/RX filtering | OK |
| TNFS mount | OK |
| TNFS opendir / readdir / closedir | OK |
| Eerste `DIR N:\` — live server entries | OK |
| DOS terugkrijgen bij fouten (geen hard hang) | OK |

---

## Bekende bug (juni 2026)

**Tweede en volgende `DIR N:\` krijgen soms geen TNFS response.**

Symptoom: TX wordt verzonden (SND ok), maar server antwoordt niet.
Eerste DIR werkt altijd.  Oorzaak nog niet gevonden.

**Volgende debugstap:**
TX Ethernet/IP/UDP frame headers dumpen voor elke send (`TXH:` in ring buffer)
en `SND ok` / `SND FAIL` loggen om te bevestigen dat de packet driver de frames
werkelijk verzendt.  Vergelijk byte-voor-byte frame van werkende closedir (seq=0D)
met falende opendir (seq=0E).

---

## Opstarten op de 486

**CONFIG.SYS** vereist `LASTDRIVE=Z` (of minimaal `=N`).

```bat
d:\3c509\pktdvr\3c5x9pd.com 0x60   ; laad 3Com packet driver op INT 60h
d:\tnfs\tnfsdrv.exe                 ; installeer TNFSDRV (toont DUMPBUF-adres)
N:                                  ; schakel naar virtuele drive
DIR                                 ; lijst root-directory
DUMPBUF SSSS:OOOO                   ; dump ring buffer (adres op scherm bij start)
```

**TNFSDRV.CFG** (in dezelfde directory als TNFSDRV.EXE):

```ini
[default]
servername=192.168.178.10
serverroot=/DOS
drive=N
port=16384
localip=192.168.178.246
packetint=0x60
```

---

## Bouwen

Vereist **Open Watcom C/C++ 2.0** (16-bit DOS, `wcc` + `wasm` + `wlink` in PATH).

```
make DEBUG=1            # packet-driver + debug ring buffer  [standaard]
make                    # packet-driver, release
make PUREFAKE=1 DEBUG=1 # geen netwerk, smoke-test
```

Produceert in `build/`:

| Bestand | Doel |
|---|---|
| `tnfsdrv.exe` | TSR — één keer laden, blijft resident |
| `dumpbuf.exe` | Ring buffer dumpen na TSR-operaties |

---

## Belangrijke bestanden

```
src/
  main.c           — TSR install: CDS setup, INT 2Fh hook, stay-resident
  handler.asm      — INT 2Fh interrupt entry, stack switch, DS restore
  redirector.c     — INT 2Fh subfuncties (FINDFIRST, FINDNEXT, GETATTR, ...)
  fs_tnfsmin.c     — Actieve FS backend: TNFS opendir/readdir cache
  tnfs.c           — TNFS protocol
  netinit.c        — Startup: packet driver init, ARP, TNFS mount
  netw_pd.c        — Ethernet/ARP/IPv4/UDP transport
  netw_pd_rcv.asm  — Packet driver receiver callback
  config.c         — TNFSDRV.CFG parser
  ringbuf.c        — Ring buffer (post-TSR debug)
  dumpbuf.c        — DUMPBUF tool
  fs_fake.c        — Fake FS (alleen voor PUREFAKE smoke-test build)

include/
  fs.h             — FS backend interface
  tnfs.h           — TNFS protocol declarations
  netw.h           — Netwerk API (send/recv/connect/...)
  netw_pd.h        — Packet driver specifieke setup + shared state
  netinit.h        — tnfsdrv_connect / tnfsdrv_disconnect
  config.h         — TnfsDrvConfig struct
  ringbuf.h        — Ring buffer API
  redirector.h     — Redirector internals

attic/
  netw.cpp         — Oude mTCP C++ network backend (niet meer gebruikt)
  fs_tnfs.c        — Volledige TNFS FS backend via mTCP (niet meer gebruikt)
  tnfsdrv.cfg      — Oud duplicate config bestand
```

---

## Technische details

### Watcom -zu (DS ≠ SS in TSR-context)
Alle TSR-code wordt gecompileerd met `-zu`.  Lokale (stack) variabelen zijn SS-relatief;
near pointers die worden doorgegeven aan niet-`-zu` functies moeten `static` zijn (DGROUP-relatief).

### Packet driver callback (pd_receiver_)
FAR CALL vanuit de packet driver interrupt.  AX=0: geef buffer terug (ES:DI = s_rx_buf of 0:0).
AX=1: frame ontvangen (CX = lengte, ES:DI = buffer).

### CDS flags: 0xC000
Zowel `CDSFLAG_NET` (0x8000) als `CDSFLAG_PHY` (0x4000) moeten gezet zijn.
MS-DOS 6.x negeert een entry met alleen het NET-bit.

### TNFS root opendir
Root directory: path="" (lege string), minimale payload 6 bytes (twee NUL-bytes).
`/` werkt niet bij herhaalde opens — server accepteert `""` consequent.

### Ring buffer / DUMPBUF
TNFSDRV print bij start het adres: `Run: DUMPBUF SSSS:OOOO`.
Na `DIR N:\`: `DUMPBUF SSSS:OOOO` om alle TX/RX/DROP/ACCEPT events te zien.

---

## Credits

- **[EtherDFS](http://etherdfs.sourceforge.net)** by Mateusz Viste — technische referentie
  voor de MS-DOS redirector API, CDS-layout en SDA-structuur.
- **[RBIL](http://www.ctyme.com/rbrown.htm)** — interrupt referentie voor INT 21h / INT 2Fh.
- **Open Watcom 2.0** — 16-bit DOS compiler en assembler.

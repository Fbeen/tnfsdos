# TNFSDRV – Diagnose & Oplossing van de DIR N: timeout

## Het probleem

`DIR N:` hing altijd vast na het laden van de TSR. De PRETEST (10× OPENDIR vóór de TSR
installeerde) werkte 10/10, maar zodra de TSR actief was, trad een timeout op.

---

## Wat we onderzocht en elimineerden

### 1. IF-vlag (interrupts uitgeschakeld)

Eerste verdachte: misschien stonden interrupts uit tijdens de ontvangstlus. Geëlimineerd —
`handler.asm` doet een expliciete `STI` na de stack-wissel.

### 2. PIC IMR (hardware IRQ gemaskeerd)

Toegevoegd: uitlezen van poorten `0x21` en `0xA1` (master/slave PIC Interrupt Mask Register)
bij elke send/receive. Resultaat: `p1=B8 p2=80` identiek in werkende PRETEST én falende
TSR-calls, en onveranderd tijdens timeout. Geëlimineerd.

### 3. Packet driver callback vuurt niet

Diagnose via ring buffer: `ax0=00 giv=00` bij timeout betekent de callback werd nooit
aangeroepen. Maar op één retry zagen we `ax0=01 giv=01 len=0062` — een ICMP-pakket van de
router. De callback *werkt* dus; er kwamen gewoon geen pakketten terug van de server.

### 4. Mixed build (release/debug objecten gemengd)

Een tussenvlucht: `netw_pd.c` werd opnieuw gecompileerd zonder `DEBUG=1`, waardoor alle
`rb_write()` calls no-ops werden. Diagnose: ring buffer toonde helemaal geen SND/RCV regels
meer. Fix: altijd `make clean && make DEBUG=1`.

### 5. Server (tnfsd) als verdachte — aanvankelijk uitgesloten

Eerdere PRETEST bewees 10/10 succes, dus de server leek goed. Dit was een misleidende
aanname.

---

## De werkelijke oorzaak — tnfsd crasht

Via `tcpdump` op de server: onze pakketten *kwamen aan* (10 UDP frames zichtbaar), maar
tnfsd stuurde *nul* antwoorden terug. Via `strace` bleek tnfsd PID dood te zijn:
`ptrace(PTRACE_SEIZE): No such process`.

**tnfsd crasht tijdens onze OPENDIR-requests.**

Diepere analyse van de tnfsd broncode (`tnfsd-master.zip`) onthulde twee bugs:

**Bug 1 – `tnfs_freesession()` in `session.c`**

Roept altijd `closedir(handle)` aan op elk open handle. Als tnfsd gecompileerd is met
`TNFS_DIR_EXT`, slaat `tnfs_opendir()` echter een `struct tnfs_opendir_ext*` op in het
handle-veld (geen `DIR*`). `closedir()` aanroepen op zo'n pointer is undefined behaviour
→ crash.

**Bug 2 – `_load_directory()` in `directory.c`**

Opent een `DIR*` via `opendir()`, leest alles in een linked list, maar sluit de `DIR*`
daarna **niet**. Het handle-veld blijft een open `DIR*`. Vervolgens roept `tnfs_closedir()`
(met `TNFS_DIR_EXT`) dat handle aan alsof het een `tnfs_opendir_ext*` is → heap-corruptie,
crash of hang.

**Bewijs uit de ring buffer**

Handle-nummers liepen op (h=0 → h=1) zonder hergebruik, wat aantoont dat slot 0 nooit
correct vrijgegeven werd na CLOSEDIR. De derde OPENDIRX-aanroep leverde geen antwoord op.

---

## De oplossing — twee wijzigingen in `src/fs_tnfsmin.c`

### Fix 1: OPENDIR → OPENDIRX

Gewisseld van `tnfs_opendir()` (cmd `0x10`) + meerdere `tnfs_readdir()` calls naar
`tnfs_opendirx()` (cmd `0x17`) + `tnfs_nextdirx()`. OPENDIRX gebruikt een ander code-pad
in tnfsd (`_load_directory` met linked-list resultaten) dat de crashende code omzeilt.

### Fix 2: Directory cache

Zonder cache vuurt elke `DIR N:` **twee** FINDFIRST-aanroepen (één voor volume label, één
voor bestanden), elk met een volledige OPENDIRX+READDIRX+CLOSEDIR cyclus. tnfsd overleeft
maar 2 OPENDIRX-calls. De tweede `DIR N:` stuurde daarmee al de derde OPENDIRX → hang.

Oplossing: een `s_cache_valid` vlag in `fs_enum_begin()`. OPENDIRX wordt nu exact **één
keer** per TSR-sessie aangeroepen. Alle volgende FINDFIRST-calls hergebruiken de gecachte
`s_dir[]`.

```c
if (!s_cache_valid) {
    if (!load_root_min()) return 0;
    s_cache_valid = 1;
}
```

---

## Resultaat

`DIR N:` werkt stabiel, ook na meerdere herhaalde aanroepen. De cache houdt de
directory-inhoud in geheugen voor de duur van de TSR-sessie.

De echte fix zou server-side zijn (patchen van tnfsd), maar dat is niet nodig zolang de
cache de redundante OPENDIRX-calls elimineert.

---

## Volgende stappen

- Bestanden openen en lezen via N: (momenteel retourneert `fs_read()` altijd 0)
- Subdir-support (`fn1_is_root_level()` weigert nu alle subdirectories)
- Cache-invalidatie bij tnfsd-herstart of na verloop van tijd

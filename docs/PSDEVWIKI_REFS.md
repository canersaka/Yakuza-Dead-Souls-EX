# psdevwiki.com/ps3 — captured references

**Access:** psdevwiki blocks automated fetch (WebFetch/API = 403, archive.org blocked here). It works
through the **Claude-in-Chrome extension** (browser navigate + get_page_text). Ask to capture specific
pages by name. Clean-room note: this is community RE documentation (facts/addresses) — reference facts,
don't copy GPL emulator source.

---

## RSX (captured 2026-07-01 from /ps3/RSX) — for LAYER 2

**Chip:** RSX "Reality Synthesizer", Nvidia **G70/G71 (NV47) hybrid**, ~GeForce 7800GTX. Little-endian.
Independent vertex/pixel shader pipelines, Shader Model 3.0, Nvidia Cg. 8 ROPs, 128-bit GDDR3 bus,
24 pixel shader ALUs @550MHz, 8 vertex pipelines. 256MB GDDR3 @650MHz + up to 224MB XDR via Cell.
=> confirms our Layer-2 target is **NV40/NV47-class (Curie)**, NOT modern NVK. Use nouveau nvfx +
envytools/rnndb (below) as the clean-room ISA/method oracle.

**RSX memory map (256MB GDDR3; last 4MB = GPU state) — useful for the FIFO/context modeling:**
| Range | Size | Contents |
|---|---|---|
| 0000000-FBFFFFF | 252 MB | Framebuffer |
| FC00000-FFFFFFF | 4 MB | GPU Data |
| FF80000-FFFFFFF | 512 KB | RAMIN: Instance Memory |
| FF90000-FF93FFF | 16 KB | RAMHT: Hash Table |
| FFA0000-FFA0FFF | 4 KB | **RAMFC: FIFO Context** |
| FFC0000-FFCFFFF | 64 KB | DMA Objects |
| FFD0000-FFDFFFF | 64 KB | Graphic Objects |
| FFE0000-FFFFFFF | 128 KB | GRAPH: Graphic Context |
RSX can also access main XDR (0..256MB or 0..512MB). Cell READ from GDDR3 is VERY slow (~16MB/s) — so
the engine works in XDR and has RSX pull from XDR (extra texture-lookup instrs for this).

**Command submission:** PSGL (OpenGL|ES) is on top of **libgcm** (the native command-buffer lib we
bridge); lowest level = FIFO Context + DMA Objects issued via DMA. (RSXFIFO page is empty on the wiki.)

**Clean-room hardware-doc refs (MIT/open — the real Layer-2 sources) linked from the page:**
- **envytools** (`nouveau/envytools`): hwdocs + **rnndb** register/method XMLs — covers the NV40 family.
- **nouveau** (nv30/nv40 = nvfx) — the classic driver for this GPU generation.
- Mesa/Gallium Cell driver, ps3rsx.git, xf86-video-ps3, rsxgl (github gzorin/RSXGL), PSL1GHT.
- Cg Toolkit manual + The Cg Tutorial (fragment/vertex shader semantics).

**ROM/Vbios:** retail RSX reports e.g. `rsx: b08 500/650 vpe:ff shd:3f` (from lv1 debug). Not needed
for emulation but confirms clock 500/650.

---

## Still to capture (name a page to pull via the browser)
- **RSXFIFOCommands** (https://www.psdevwiki.com/ps3/RSXFIFOCommands) — ★ HIGH PRIORITY for Layer 2:
  the NV4097-class FIFO method/command numbers we must dispatch into the D3D12 backend (surface setup,
  CLEAR, draws, texture upload). Attempted 2026-07-01 but the Chrome extension dropped mid-session;
  retry next session. (RSXFIFO itself is an empty wiki page; this is the real command reference.)
- **LibGCM** — the guest gcm API surface we bridge (methods, context, flip/display-buffer).
- **Hypervisor / lv1 / lv2 syscall list** — syscall numbers/signatures (we hit wrong-number bugs).
- **SPU / SPU Details / MFC** — LS layout, channels, DMA, preferred-slot (cross-check lifter).
- RSX method/register specifics likely live under a page like "Hardware/RSX" or in envytools rnndb
  rather than a wiki page — envytools rnndb is the better source for NV4097 method numbers.

"""
PS3 ELF / PRX binary parser (library module).

Parses ELF64 (PPU) and ELF32 headers, program headers, section headers,
and the PRX module info with its export/import tables. Input files must
already be decrypted ELFs.
"""

import struct

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# ELF magic
ELF_MAGIC = b"\x7fELF"

# ELF class
ELFCLASS64 = 2

# Endianness
ELFDATA2MSB = 2  # big-endian  (PS3 native)

# ELF types
ET_SCE_PPURELEXEC = 0xFFA4  # PRX

# Program header types
PT_LOAD = 1

# ---------------------------------------------------------------------------
# Helper: endian-aware unpacker
# ---------------------------------------------------------------------------

class BinaryReader:
    """Thin wrapper around a bytes buffer with endian-aware reads."""

    def __init__(self, data: bytes, big_endian: bool = True):
        self.data = data
        self.endian = ">" if big_endian else "<"

    def u8(self, off: int) -> int:
        return self.data[off]

    def u16(self, off: int) -> int:
        return struct.unpack_from(self.endian + "H", self.data, off)[0]

    def u32(self, off: int) -> int:
        return struct.unpack_from(self.endian + "I", self.data, off)[0]

    def u64(self, off: int) -> int:
        return struct.unpack_from(self.endian + "Q", self.data, off)[0]

    def cstring(self, off: int, max_len: int = 256) -> str:
        end = self.data.find(b"\x00", off, off + max_len)
        if end == -1:
            end = off + max_len
        return self.data[off:end].decode("utf-8", errors="replace")

# ---------------------------------------------------------------------------
# ELF header
# ---------------------------------------------------------------------------

class ELFHeader:
    """ELF32 / ELF64 file header."""

    def __init__(self):
        self.ei_class = 0      # 1=32, 2=64
        self.ei_data = 0       # 1=LE, 2=BE
        self.ei_osabi = 0
        self.e_type = 0
        self.e_machine = 0
        self.e_version = 0
        self.e_entry = 0
        self.e_phoff = 0
        self.e_shoff = 0
        self.e_flags = 0
        self.e_ehsize = 0
        self.e_phentsize = 0
        self.e_phnum = 0
        self.e_shentsize = 0
        self.e_shnum = 0
        self.e_shstrndx = 0
        self.big_endian = True
        self.is64 = True

    @classmethod
    def parse(cls, data: bytes, offset: int = 0) -> "ELFHeader":
        if data[offset:offset + 4] != ELF_MAGIC:
            raise ValueError(f"Bad ELF magic at offset {offset}")

        hdr = cls()
        hdr.ei_class = data[offset + 4]
        hdr.ei_data = data[offset + 5]
        hdr.ei_osabi = data[offset + 7]
        hdr.is64 = hdr.ei_class == ELFCLASS64
        hdr.big_endian = hdr.ei_data == ELFDATA2MSB

        rd = BinaryReader(data, hdr.big_endian)
        off = offset

        hdr.e_type = rd.u16(off + 16)
        hdr.e_machine = rd.u16(off + 18)
        hdr.e_version = rd.u32(off + 20)

        if hdr.is64:
            hdr.e_entry = rd.u64(off + 24)
            hdr.e_phoff = rd.u64(off + 32)
            hdr.e_shoff = rd.u64(off + 40)
            hdr.e_flags = rd.u32(off + 48)
            hdr.e_ehsize = rd.u16(off + 52)
            hdr.e_phentsize = rd.u16(off + 54)
            hdr.e_phnum = rd.u16(off + 56)
            hdr.e_shentsize = rd.u16(off + 58)
            hdr.e_shnum = rd.u16(off + 60)
            hdr.e_shstrndx = rd.u16(off + 62)
        else:
            hdr.e_entry = rd.u32(off + 24)
            hdr.e_phoff = rd.u32(off + 28)
            hdr.e_shoff = rd.u32(off + 32)
            hdr.e_flags = rd.u32(off + 36)
            hdr.e_ehsize = rd.u16(off + 40)
            hdr.e_phentsize = rd.u16(off + 42)
            hdr.e_phnum = rd.u16(off + 44)
            hdr.e_shentsize = rd.u16(off + 46)
            hdr.e_shnum = rd.u16(off + 48)
            hdr.e_shstrndx = rd.u16(off + 50)

        return hdr

# ---------------------------------------------------------------------------
# Program header
# ---------------------------------------------------------------------------

class ProgramHeader:
    """Single ELF program header (PHDR) entry."""

    def __init__(self):
        self.p_type = 0
        self.p_flags = 0
        self.p_offset = 0
        self.p_vaddr = 0
        self.p_paddr = 0
        self.p_filesz = 0
        self.p_memsz = 0
        self.p_align = 0

    @classmethod
    def parse(cls, data: bytes, offset: int, is64: bool, big_endian: bool) -> "ProgramHeader":
        rd = BinaryReader(data, big_endian)
        ph = cls()
        off = offset
        if is64:
            ph.p_type = rd.u32(off + 0)
            ph.p_flags = rd.u32(off + 4)
            ph.p_offset = rd.u64(off + 8)
            ph.p_vaddr = rd.u64(off + 16)
            ph.p_paddr = rd.u64(off + 24)
            ph.p_filesz = rd.u64(off + 32)
            ph.p_memsz = rd.u64(off + 40)
            ph.p_align = rd.u64(off + 48)
        else:
            ph.p_type = rd.u32(off + 0)
            ph.p_offset = rd.u32(off + 4)
            ph.p_vaddr = rd.u32(off + 8)
            ph.p_paddr = rd.u32(off + 12)
            ph.p_filesz = rd.u32(off + 16)
            ph.p_memsz = rd.u32(off + 20)
            ph.p_flags = rd.u32(off + 24)
            ph.p_align = rd.u32(off + 28)
        return ph

# ---------------------------------------------------------------------------
# Section header
# ---------------------------------------------------------------------------

class SectionHeader:
    """Single ELF section header entry."""

    def __init__(self):
        self.sh_name = 0
        self.sh_type = 0
        self.sh_flags = 0
        self.sh_addr = 0
        self.sh_offset = 0
        self.sh_size = 0
        self.sh_link = 0
        self.sh_info = 0
        self.sh_addralign = 0
        self.sh_entsize = 0
        self.name_str = ""

    @classmethod
    def parse(cls, data: bytes, offset: int, is64: bool, big_endian: bool) -> "SectionHeader":
        rd = BinaryReader(data, big_endian)
        sh = cls()
        off = offset
        if is64:
            sh.sh_name = rd.u32(off + 0)
            sh.sh_type = rd.u32(off + 4)
            sh.sh_flags = rd.u64(off + 8)
            sh.sh_addr = rd.u64(off + 16)
            sh.sh_offset = rd.u64(off + 24)
            sh.sh_size = rd.u64(off + 32)
            sh.sh_link = rd.u32(off + 40)
            sh.sh_info = rd.u32(off + 44)
            sh.sh_addralign = rd.u64(off + 48)
            sh.sh_entsize = rd.u64(off + 56)
        else:
            sh.sh_name = rd.u32(off + 0)
            sh.sh_type = rd.u32(off + 4)
            sh.sh_flags = rd.u32(off + 8)
            sh.sh_addr = rd.u32(off + 12)
            sh.sh_offset = rd.u32(off + 16)
            sh.sh_size = rd.u32(off + 20)
            sh.sh_link = rd.u32(off + 24)
            sh.sh_info = rd.u32(off + 28)
            sh.sh_addralign = rd.u32(off + 32)
            sh.sh_entsize = rd.u32(off + 36)
        return sh

# ---------------------------------------------------------------------------
# PRX module info
# ---------------------------------------------------------------------------

class PRXModuleInfo:
    """PS3 PRX sys_process_prx_info_t structure.

    Typically located at the start of the first PT_LOAD segment for PRX files.
    Layout (big-endian, 64-bit pointers):
        u16  attributes
        u8   version[2]
        char name[28]
        u32  toc
        u64  exports_start
        u64  exports_end
        u64  imports_start
        u64  imports_end
    """

    SIZE = 0x48

    def __init__(self):
        self.attributes = 0
        self.version = (0, 0)
        self.name = ""
        self.toc = 0
        self.exports_start = 0
        self.exports_end = 0
        self.imports_start = 0
        self.imports_end = 0

    @classmethod
    def parse(cls, data: bytes, offset: int, big_endian: bool = True) -> "PRXModuleInfo":
        if offset + cls.SIZE > len(data):
            raise ValueError("Not enough data for PRX module info")
        rd = BinaryReader(data, big_endian)
        m = cls()
        m.attributes = rd.u16(offset)
        m.version = (rd.u8(offset + 2), rd.u8(offset + 3))
        m.name = rd.cstring(offset + 4, 28)
        m.toc = rd.u32(offset + 32)
        m.exports_start = rd.u64(offset + 36)
        m.exports_end = rd.u64(offset + 44)
        m.imports_start = rd.u64(offset + 52)
        m.imports_end = rd.u64(offset + 60)
        return m

# ---------------------------------------------------------------------------
# PRX export / import table entry helpers
# ---------------------------------------------------------------------------

class PRXExportEntry:
    """PRX export table entry (variable size, 0x1C minimum)."""

    SIZE_MIN = 0x1C

    def __init__(self):
        self.size = 0
        self.attributes = 0
        self.version = 0
        self.num_funcs = 0
        self.num_vars = 0
        self.num_tlsvars = 0
        self.name_ptr = 0
        self.nid_table_ptr = 0
        self.stub_table_ptr = 0
        self.name_str = ""
        self.nids: list[int] = []

    @classmethod
    def parse(cls, data: bytes, offset: int, big_endian: bool = True) -> "PRXExportEntry":
        rd = BinaryReader(data, big_endian)
        e = cls()
        e.size = rd.u8(offset + 0)
        e.attributes = rd.u8(offset + 1) << 8 | rd.u8(offset + 2) if e.size > 2 else 0
        # Typical layout:
        e.version = rd.u16(offset + 2) if e.size >= 4 else 0
        e.attributes = rd.u16(offset + 4) if e.size >= 6 else 0
        e.num_funcs = rd.u16(offset + 6) if e.size >= 8 else 0
        e.num_vars = rd.u16(offset + 8) if e.size >= 10 else 0
        e.num_tlsvars = rd.u16(offset + 10) if e.size >= 12 else 0
        # skip reserved
        e.name_ptr = rd.u32(offset + 16) if e.size >= 20 else 0
        e.nid_table_ptr = rd.u32(offset + 20) if e.size >= 24 else 0
        e.stub_table_ptr = rd.u32(offset + 24) if e.size >= 28 else 0
        return e


class PRXImportEntry:
    """PRX import table entry."""

    SIZE_MIN = 0x2C

    def __init__(self):
        self.size = 0
        self.attributes = 0
        self.version = 0
        self.num_funcs = 0
        self.num_vars = 0
        self.num_tlsvars = 0
        self.name_ptr = 0
        self.nid_table_ptr = 0
        self.stub_table_ptr = 0
        self.name_str = ""
        self.nids: list[int] = []

    @classmethod
    def parse(cls, data: bytes, offset: int, big_endian: bool = True) -> "PRXImportEntry":
        rd = BinaryReader(data, big_endian)
        i = cls()
        i.size = rd.u8(offset + 0)
        i.version = rd.u16(offset + 2) if i.size >= 4 else 0
        i.attributes = rd.u16(offset + 4) if i.size >= 6 else 0
        i.num_funcs = rd.u16(offset + 6) if i.size >= 8 else 0
        i.num_vars = rd.u16(offset + 8) if i.size >= 10 else 0
        i.num_tlsvars = rd.u16(offset + 10) if i.size >= 12 else 0
        # reserved[4]
        i.name_ptr = rd.u32(offset + 16) if i.size >= 20 else 0
        i.nid_table_ptr = rd.u32(offset + 20) if i.size >= 24 else 0
        i.stub_table_ptr = rd.u32(offset + 24) if i.size >= 28 else 0
        return i

# ---------------------------------------------------------------------------
# Top-level ELF file
# ---------------------------------------------------------------------------

def vaddr_to_offset(phdrs: list[ProgramHeader], vaddr: int) -> int | None:
    """Convert a virtual address to a file offset using PT_LOAD segments."""
    for ph in phdrs:
        if ph.p_type == PT_LOAD and ph.p_filesz > 0:
            if ph.p_vaddr <= vaddr < ph.p_vaddr + ph.p_filesz:
                return int(ph.p_offset + (vaddr - ph.p_vaddr))
    return None


class ELFFile:
    """Represents a parsed (decrypted) PS3 ELF / PRX file."""

    def __init__(self, path: str):
        self.path = path
        self.raw_data: bytes = b""
        self.elf_offset: int = 0
        self.elf_header: ELFHeader | None = None
        self.program_headers: list[ProgramHeader] = []
        self.section_headers: list[SectionHeader] = []
        self.module_info: PRXModuleInfo | None = None
        self.exports: list[PRXExportEntry] = []
        self.imports: list[PRXImportEntry] = []

    # ---- loading ----

    def load(self) -> None:
        """Read the file and parse all structures."""
        with open(self.path, "rb") as f:
            self.raw_data = f.read()

        # Parse ELF header
        if self.raw_data[self.elf_offset:self.elf_offset + 4] != ELF_MAGIC:
            raise ValueError("Could not locate ELF header in file")

        self.elf_header = ELFHeader.parse(self.raw_data, self.elf_offset)
        self._parse_program_headers()
        self._parse_section_headers()
        self._resolve_section_names()

        # If this is a PRX, parse module info
        if self.elf_header.e_type == ET_SCE_PPURELEXEC:
            self._parse_prx_module_info()

    # ---- internal parsing ----

    def _rd(self) -> BinaryReader:
        return BinaryReader(self.raw_data, self.elf_header.big_endian)

    def _parse_program_headers(self) -> None:
        eh = self.elf_header
        base = self.elf_offset + eh.e_phoff
        for i in range(eh.e_phnum):
            off = base + i * eh.e_phentsize
            if off + eh.e_phentsize > len(self.raw_data):
                break
            ph = ProgramHeader.parse(self.raw_data, off, eh.is64, eh.big_endian)
            self.program_headers.append(ph)

    def _parse_section_headers(self) -> None:
        eh = self.elf_header
        if eh.e_shoff == 0 or eh.e_shnum == 0:
            return
        base = self.elf_offset + eh.e_shoff
        for i in range(eh.e_shnum):
            off = base + i * eh.e_shentsize
            if off + eh.e_shentsize > len(self.raw_data):
                break
            sh = SectionHeader.parse(self.raw_data, off, eh.is64, eh.big_endian)
            self.section_headers.append(sh)

    def _resolve_section_names(self) -> None:
        eh = self.elf_header
        if eh.e_shstrndx == 0 or eh.e_shstrndx >= len(self.section_headers):
            return
        strtab_sh = self.section_headers[eh.e_shstrndx]
        strtab_off = self.elf_offset + strtab_sh.sh_offset
        rd = BinaryReader(self.raw_data, eh.big_endian)
        for sh in self.section_headers:
            if sh.sh_name < strtab_sh.sh_size:
                sh.name_str = rd.cstring(strtab_off + sh.sh_name)

    def _parse_prx_module_info(self) -> None:
        """Extract module info from the first PT_LOAD segment of a PRX."""
        for ph in self.program_headers:
            if ph.p_type == PT_LOAD and ph.p_filesz > 0:
                # module info is at paddr offset within the segment for PRX
                info_off = int(ph.p_offset + (ph.p_paddr & 0xFFFFFFFF))
                if info_off + PRXModuleInfo.SIZE <= len(self.raw_data):
                    try:
                        self.module_info = PRXModuleInfo.parse(
                            self.raw_data, info_off, self.elf_header.big_endian
                        )
                        self._parse_exports_imports()
                    except Exception:
                        pass
                break

    def _parse_exports_imports(self) -> None:
        mi = self.module_info
        rd = self._rd()

        # exports
        if mi.exports_start and mi.exports_end and mi.exports_end > mi.exports_start:
            off = vaddr_to_offset(self.program_headers, mi.exports_start)
            end_off = vaddr_to_offset(self.program_headers, mi.exports_end)
            if off is not None and end_off is not None:
                while off < end_off and off + PRXExportEntry.SIZE_MIN <= len(self.raw_data):
                    entry = PRXExportEntry.parse(self.raw_data, off, self.elf_header.big_endian)
                    if entry.size == 0:
                        break
                    # resolve name
                    if entry.name_ptr:
                        name_off = vaddr_to_offset(self.program_headers, entry.name_ptr)
                        if name_off is not None:
                            entry.name_str = rd.cstring(name_off)
                    # read NID table
                    total = entry.num_funcs + entry.num_vars + entry.num_tlsvars
                    if entry.nid_table_ptr and total > 0:
                        nid_off = vaddr_to_offset(self.program_headers, entry.nid_table_ptr)
                        if nid_off is not None:
                            for j in range(total):
                                if nid_off + j * 4 + 4 <= len(self.raw_data):
                                    entry.nids.append(rd.u32(nid_off + j * 4))
                    self.exports.append(entry)
                    off += entry.size

        # imports
        if mi.imports_start and mi.imports_end and mi.imports_end > mi.imports_start:
            off = vaddr_to_offset(self.program_headers, mi.imports_start)
            end_off = vaddr_to_offset(self.program_headers, mi.imports_end)
            if off is not None and end_off is not None:
                while off < end_off and off + PRXImportEntry.SIZE_MIN <= len(self.raw_data):
                    entry = PRXImportEntry.parse(self.raw_data, off, self.elf_header.big_endian)
                    if entry.size == 0:
                        break
                    if entry.name_ptr:
                        name_off = vaddr_to_offset(self.program_headers, entry.name_ptr)
                        if name_off is not None:
                            entry.name_str = rd.cstring(name_off)
                    total = entry.num_funcs + entry.num_vars + entry.num_tlsvars
                    if entry.nid_table_ptr and total > 0:
                        nid_off = vaddr_to_offset(self.program_headers, entry.nid_table_ptr)
                        if nid_off is not None:
                            for j in range(total):
                                if nid_off + j * 4 + 4 <= len(self.raw_data):
                                    entry.nids.append(rd.u32(nid_off + j * 4))
                    self.imports.append(entry)
                    off += entry.size

    # ---- public query helpers ----

    def get_segment_data(self, index: int) -> bytes:
        """Return the raw bytes of program header *index*."""
        ph = self.program_headers[index]
        return self.raw_data[ph.p_offset:ph.p_offset + ph.p_filesz]

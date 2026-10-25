// PE32 loader for the native host: maps the image at its preferred base inside guest memory and points every IAT
// slot at an import thunk.
#include <sys/mman.h>

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "guest.h"

namespace native {

namespace {

#pragma pack(push, 1)
struct DosHeader {
    uint16_t magic;
    uint8_t pad[58];
    uint32_t lfanew;
};
struct FileHeader {
    uint16_t machine, sections;
    uint32_t stamp, symtab, symbols;
    uint16_t opt_size, characteristics;
};
struct DataDir {
    uint32_t rva, size;
};
struct OptHeader32 {
    uint16_t magic;
    uint8_t lmaj, lmin;
    uint32_t code_size, init_size, uninit_size, entry, code_base, data_base, image_base, sect_align, file_align;
    uint16_t os_maj, os_min, img_maj, img_min, sub_maj, sub_min;
    uint32_t win32ver, image_size, headers_size, checksum;
    uint16_t subsystem, dll_chars;
    uint32_t stack_res, stack_commit, heap_res, heap_commit, loader_flags, dirs;
    DataDir dir[16];
};
struct Section {
    char name[8];
    uint32_t vsize, va, raw_size, raw_ptr, reloc_ptr, line_ptr;
    uint16_t nreloc, nline;
    uint32_t characteristics;
};
struct ImportDesc {
    uint32_t lookup, stamp, chain, name, iat;
};
struct ExportDir {
    uint32_t flags, stamp;
    uint16_t maj, min;
    uint32_t name, base, nfuncs, nnames, funcs, names, ordinals;
};
#pragma pack(pop)

std::string Lower(const char* s) {
    std::string r(s);
    for (char& ch : r) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    return r;
}

}  // namespace

bool LoadImage(const char* path, LoadedImage* out, uint32_t load_base) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        Log("loader: cannot open %s", path);
        return false;
    }
    std::vector<uint8_t> file;
    fseek(f, 0, SEEK_END);
    file.resize(static_cast<size_t>(ftell(f)));
    fseek(f, 0, SEEK_SET);
    size_t got = fread(file.data(), 1, file.size(), f);
    fclose(f);
    if (got != file.size() || file.size() < sizeof(DosHeader)) {
        return false;
    }
    auto* dos = reinterpret_cast<DosHeader*>(file.data());
    auto* fh = reinterpret_cast<FileHeader*>(file.data() + dos->lfanew + 4);
    auto* opt = reinterpret_cast<OptHeader32*>(fh + 1);
    auto* sections = reinterpret_cast<Section*>(reinterpret_cast<uint8_t*>(opt) + fh->opt_size);
    if (dos->magic != 0x5a4d || opt->magic != 0x10b) {
        Log("loader: %s is not a PE32 image", path);
        return false;
    }
    const uint32_t base = load_base ? load_base : opt->image_base;
    const uint32_t size = (opt->image_size + 0xfff) & ~0xfffu;
    // Inside the guest window InitGuestMemory reserved (kept out of the arena), so MAP_FIXED replaces only that.
    void* p = mmap(Ptr<void>(base), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p != Ptr<void>(base)) {
        Log("loader: cannot map %s at %08x", path, base);
        return false;
    }
    memcpy(Ptr<void>(base), file.data(), opt->headers_size);
    for (int i = 0; i < fh->sections; ++i) {
        const Section& s = sections[i];
        uint32_t n = s.raw_size < s.vsize ? s.raw_size : s.vsize;
        if (n && s.raw_ptr) {
            memcpy(Ptr<void>(base + s.va), file.data() + s.raw_ptr, n);
        }
    }
    // Base relocations when the image does not sit at its preferred base: IMAGE_REL_BASED_HIGHLOW entries get the
    // delta (the recompiler applied the same ones to the code it generated, tools/recomp/recomp.py --base).
    if (base != opt->image_base) {
        const DataDir& rd = opt->dir[5];
        if (!rd.rva) {
            Log("loader: %s has no relocations, cannot load it at %08x", path, base);
            return false;
        }
        const uint32_t delta = base - opt->image_base;
        int fixups = 0;
        for (uint32_t blk = base + rd.rva; blk < base + rd.rva + rd.size;) {
            uint32_t page = R32(blk), block_size = R32(blk + 4);
            if (block_size < 8) break;
            for (uint32_t e = blk + 8; e < blk + block_size; e += 2) {
                uint16_t v = R16(e);
                if ((v >> 12) == 3) {  // HIGHLOW
                    uint32_t at = base + page + (v & 0xfff);
                    W32(at, R32(at) + delta);
                    ++fixups;
                } else if ((v >> 12) != 0) {  // ABSOLUTE (padding) is the only other type in a PE32 DLL
                    Log("loader: unsupported relocation type %u", v >> 12);
                    return false;
                }
            }
            blk += block_size;
        }
        Ptr<OptHeader32>(base + dos->lfanew + 4 + sizeof(FileHeader))->image_base = base;
        Log("loader: rebased to %08x (%d fixups)", base, fixups);
    }
    // Imports: every IAT slot gets the thunk of its dll!name (data imports are handled by ImportThunk too).
    const DataDir& id = opt->dir[1];
    int count = 0;
    for (auto* d = Ptr<ImportDesc>(base + id.rva); id.rva && d->name; ++d) {
        std::string dll = Lower(Str(base + d->name));
        uint32_t lookup = d->lookup ? d->lookup : d->chain;
        for (uint32_t k = 0;; ++k) {
            uint32_t entry = R32(base + lookup + 4 * k);
            if (!entry) break;
            char ord[16];
            const char* name;
            if (entry & 0x80000000u) {
                snprintf(ord, sizeof(ord), "#%u", entry & 0xffff);
                name = ord;
            } else {
                name = Str(base + entry + 2);
            }
            W32(base + d->iat + 4 * k, ImportThunk(dll.c_str(), name));
            ++count;
        }
    }
    out->base = base;
    out->size = size;
    out->entry = opt->entry ? base + opt->entry : 0;
    Log("loader: %s at %08x-%08x, %d imports, entry %08x", path, base, base + size, count, out->entry);
    return true;
}

uint32_t FindExport(const LoadedImage& image, const char* name) {
    auto* dos = Ptr<DosHeader>(image.base);
    auto* opt = Ptr<OptHeader32>(image.base + dos->lfanew + 4 + sizeof(FileHeader));
    const DataDir& ed = opt->dir[0];
    if (!ed.rva) return 0;
    auto* e = Ptr<ExportDir>(image.base + ed.rva);
    for (uint32_t i = 0; i < e->nnames; ++i) {
        if (strcmp(Str(image.base + R32(image.base + e->names + 4 * i)), name) == 0) {
            uint16_t ord = R16(image.base + e->ordinals + 2 * i);
            return image.base + R32(image.base + e->funcs + 4 * ord);
        }
    }
    return 0;
}

}  // namespace native

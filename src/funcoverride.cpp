/* Quibble UEFI loader
 *
 * Function overrides.
 *
 * From Windows 11 24H2 the kernel image can contain "function overrides": a
 * dynamic value relocation table entry (IMAGE_DYNAMIC_RELOCATION_FUNCTION_OVERRIDE)
 * which lists the call sites of a few functions (memset, KeCopyPage, ...) and
 * a number of alternative implementations, and a decision diagram that picks
 * one of them depending on what the CPU can do. The boot loader has to apply
 * this to ntoskrnl.exe and hal.dll, by rewriting the rel32 of each call site;
 * the kernel does the same for the drivers it loads, and bugchecks
 * (MEMORY_MANAGEMENT, 0x301) in MiInitializeFunctionOverrides if the loader
 * hasn't, which it tests by calling RtlFunctionOverrideSelfTest.
 *
 * The features the diagrams are tested against are worked out here in the
 * same way as the kernel does in RtlpInitFunctionOverrideCapabilities, as
 * the kernel does it again later, for the drivers.
 *
 * This is free software; see the licence of the rest of Quibble. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <initializer_list>
#include "peload.h"
#include "peloaddef.h"
#include "misc.h"
#include "quibble.h"
#include "print.h"
#include "funcoverride.h"

// Total size of the KSCP sections (kernel/HAL retpoline and CFG dispatch code) of ntoskrnl.exe and hal.dll,
// rounded up to pages. The kernel, in MiPrepareScpFixupsForNtAndHal, refuses to set up its KSCP section unless
// the loader extension's reserve size (RetpolineReserveSize, later KernelScpReserveSize) is at least that big.
uint32_t kernel_scp_reserve_size = 0;

#ifdef __x86_64__

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif

#define FO_DVRT_FUNCTION_OVERRIDE 7
#define FO_RELOC_X64_REL32 1
#define FO_MAX_FEATURE 0x161
#define FO_BITMAP_BYTES 0x2d

#define FO_NONE 0xffffffff

// The kernel keeps a bitmap for user mode (first) and one for kernel mode, each indexed by feature number.
// We only patch the kernel and HAL, so only need the kernel one.
struct fo_caps {
    uint8_t kernel[FO_BITMAP_BYTES];
};

struct fo_cpu_feature {
    uint32_t leaf;
    uint32_t subleaf;
    uint64_t xstate;
    uint32_t mask;
    uint32_t reg;
    uint32_t flags; // 1 = user mode, 2 = kernel mode
    uint32_t feature;
};

// RtlpCpuFeatureTable
static const fo_cpu_feature cpu_features[] = {
    { 1, 0, 0x0, 0x00080000, 2, 3, 324 }, // SSE4.1
    { 1, 0, 0x4, 0x10000000, 2, 1, 316 }, // AVX
    { 7, 0, 0x4, 0x00000020, 1, 1, 317 }, // AVX2
    { 7, 0, 0x0, 0x00000200, 1, 3, 0 },   // ERMS
    { 7, 0, 0xe0, 0x00010000, 1, 1, 318 }, // AVX512F
    { 7, 0, 0x0, 0x00000010, 3, 3, 1 },   // FSRM
    { 7, 1, 0x0, 0x00000400, 0, 3, 2 },
    { 7, 1, 0x0, 0x00000800, 0, 3, 3 },
    { 7, 1, 0x0, 0x00001000, 0, 3, 4 },
    { 7, 0, 0x0, 0x00000080, 1, 3, 325 }, // SMEP
};

static void fo_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t* r) {
#ifdef _MSC_VER
    int i[4];

    __cpuidex(i, leaf, subleaf);

    r[0] = i[0];
    r[1] = i[1];
    r[2] = i[2];
    r[3] = i[3];
#else
    __cpuid_count(leaf, subleaf, r[0], r[1], r[2], r[3]);
#endif
}

static void fo_set(fo_caps& c, unsigned int feature) {
    if (feature < FO_MAX_FEATURE)
        c.kernel[feature >> 3] |= 1 << (feature & 7);
}

static bool fo_has(const fo_caps& c, unsigned int feature) {
    return feature < FO_MAX_FEATURE && (c.kernel[feature >> 3] & (1 << (feature & 7)));
}

static bool hypervisor_present() {
    uint32_t r[4];

    fo_cpuid(1, 0, r);

    if (!(r[2] & 0x80000000))
        return false;

    fo_cpuid(0x40000001, 0, r);

    return r[0] != 0x766e6258; // not the Xbox nanovisor
}

static bool hypervisor_ms_compatible() {
    uint32_t r[4];

    if (!hypervisor_present())
        return false;

    fo_cpuid(0x40000000, 0, r);

    if (r[0] < 0x40000001)
        return false;

    fo_cpuid(0x40000001, 0, r);

    return r[0] == 0x31237648; // "Hv#1"
}

// Port of RtlpInitFunctionOverrideCapabilities
static void fo_init_caps(fo_caps& c) {
    uint32_t r[4], regs[4] = { 0, 0, 0, 0 }, last_leaf = 0, max_basic, max_ext, sig;
    char vendor_string[12];
    unsigned int vendor;

    memset(&c, 0, sizeof(c));

    fo_cpuid(0, 0, r);
    max_basic = r[0];
    memcpy(vendor_string, &r[1], sizeof(uint32_t));
    memcpy(vendor_string + 4, &r[3], sizeof(uint32_t));
    memcpy(vendor_string + 8, &r[2], sizeof(uint32_t));

    fo_cpuid(0x80000000, 0, r);
    max_ext = r[0];

    fo_cpuid(1, 0, r);
    sig = r[0];

    if (!memcmp(vendor_string, "AuthenticAMD", 12) || !memcmp(vendor_string, "HygonGenuine", 12))
        vendor = 1;
    else if (!memcmp(vendor_string, "GenuineIntel", 12))
        vendor = 2;
    else if (!memcmp(vendor_string, "CentaurHauls", 12) || !memcmp(vendor_string, "  Shanghai  ", 12))
        vendor = 3;
    else
        vendor = 0;

    for (unsigned int i = 0; i < sizeof(cpu_features) / sizeof(cpu_features[0]); i++) {
        const fo_cpu_feature& f = cpu_features[i];

        // As in the kernel, the result is reused if the leaf is the same as the last one, even if the subleaf isn't
        if (f.leaf != last_leaf) {
            last_leaf = f.leaf;

            if ((f.leaf >= 0x80000000 && f.leaf <= max_ext) || (f.leaf < 0x80000000 && f.leaf <= max_basic))
                fo_cpuid(f.leaf, f.subleaf, regs);
            else
                memset(regs, 0, sizeof(regs));
        }

        if (!(regs[f.reg] & f.mask))
            continue;

        if (f.xstate != 0)
            continue; // not enabled in the kernel when we get here

        if (f.flags & 2)
            fo_set(c, f.feature);
    }

    if (vendor != 0) {
        fo_set(c, vendor == 1 ? 9 : (vendor == 2 ? 8 : 10));
        fo_set(c, 7);
    }

    fo_set(c, 0xb + ((sig >> 4) & 0xf));
    fo_set(c, 0x1b + ((sig >> 16) & 0xf));
    fo_set(c, 0x2b + ((sig >> 8) & 0xf));
    fo_set(c, 0x3b + ((sig >> 20) & 0xff));

    fo_set(c, 6);
    fo_set(c, 0x142);
    fo_set(c, 0x146);

    {
        bool set_147 = true;

        if (hypervisor_present()) {
            set_147 = false;

            if (hypervisor_ms_compatible()) {
                fo_cpuid(0x40000003, 0, r);

                if (r[1] & (1 << 12)) { // bit 44 of the features
                    fo_cpuid(0x40000004, 0, r);

                    if (!(r[0] & (1 << 12)))
                        set_147 = true;
                }
            }
        }

        if (set_147)
            fo_set(c, 0x147);
    }

    fo_set(c, 0x13b);
    fo_set(c, 0x13f);
    fo_set(c, 0x140);
}

// Walk the decision diagram. Each node is a WORD for the FALSE edge, a WORD for the TRUE edge, and a DWORD
// which is the feature number to test; a node pointing at itself with both edges is a result, with the
// DWORD being the index of the replacement, and one pointing at the first node with both edges (and not
// being it) means that the original function is kept.
static uint32_t fo_evaluate(const uint8_t* nodes, uint32_t count, const fo_caps& caps) {
    uint32_t i = 0;

    for (uint32_t steps = 0; steps <= count; steps++) {
        uint16_t left = *(uint16_t*)(nodes + (i * 8));
        uint16_t right = *(uint16_t*)(nodes + (i * 8) + 2);
        uint32_t value = *(uint32_t*)(nodes + (i * 8) + 4);

        if (left >= count || right >= count)
            return FO_NONE;

        if (left == i && right == i)
            return value;

        if (left == 0 && right == 0 && value == 0 && i != 0)
            return FO_NONE;

        i = fo_has(caps, value) ? right : left;
    }

    return FO_NONE;
}

static bool is_kernel_or_hal(uint8_t* image, uint32_t size, IMAGE_NT_HEADERS* nt) {
    IMAGE_DATA_DIRECTORY* dd = &nt->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY* exp;
    const char* name;

    if (nt->OptionalHeader64.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT || dd->VirtualAddress == 0 ||
        dd->Size < sizeof(IMAGE_EXPORT_DIRECTORY) || dd->VirtualAddress + sizeof(IMAGE_EXPORT_DIRECTORY) > size)
        return false;

    exp = (IMAGE_EXPORT_DIRECTORY*)(image + dd->VirtualAddress);

    if (exp->Name == 0 || exp->Name + 16 > size)
        return false;

    name = (const char*)(image + exp->Name);

    for (const char* n : { "ntoskrnl.exe", "hal.dll" }) {
        unsigned int i = 0;

        while (n[i] && ((name[i] | 0x20) == n[i]))
            i++;

        if (!n[i] && !name[i])
            return true;
    }

    return false;
}

static unsigned int apply(uint8_t* image, uint32_t size, uint8_t* block, uint32_t block_size, const fo_caps& caps) {
    uint32_t func_size, count = 0;
    uint8_t *q, *end, *bdds;

    if (block_size < sizeof(uint32_t))
        return 0;

    func_size = *(uint32_t*)block;

    if (func_size > block_size - sizeof(uint32_t))
        return 0;

    q = block + sizeof(uint32_t);
    end = q + func_size;
    bdds = end;

    while (q + 16 <= end) {
        uint32_t orig = ((uint32_t*)q)[0], bdd_offset = ((uint32_t*)q)[1];
        uint32_t rva_size = ((uint32_t*)q)[2], reloc_size = ((uint32_t*)q)[3];
        uint32_t* rvas = (uint32_t*)(q + 16);
        uint8_t* relocs = q + 16 + rva_size;
        uint8_t *bdd, *rel_end;
        uint32_t bdd_size, selected, target;

        if (rva_size > (uint32_t)(end - q - 16) || reloc_size > (uint32_t)(end - relocs))
            break;

        (void)orig;

        if (bdd_offset + 8 > (uint32_t)((block + block_size) - bdds)) {
            q = relocs + reloc_size;
            continue;
        }

        bdd = bdds + bdd_offset;
        bdd_size = ((uint32_t*)bdd)[1];

        if (bdd_size > (uint32_t)((block + block_size) - bdd) - 8) {
            q = relocs + reloc_size;
            continue;
        }

        selected = fo_evaluate(bdd + 8, bdd_size / 8, caps);

        if (selected != FO_NONE && selected < rva_size / sizeof(uint32_t)) {
            uint8_t* r = relocs;

            target = rvas[selected];
            rel_end = relocs + reloc_size;

            while (r + 8 <= rel_end) {
                uint32_t page = ((uint32_t*)r)[0], block_len = ((uint32_t*)r)[1];
                uint16_t* entries = (uint16_t*)(r + 8);

                if (block_len < 8 || block_len > (uint32_t)(rel_end - r))
                    break;

                for (unsigned int i = 0; i < (block_len - 8) / sizeof(uint16_t); i++) {
                    uint16_t e = entries[i];
                    uint32_t site;

                    if ((e >> 12) != FO_RELOC_X64_REL32)
                        continue;

                    site = page + (e & 0xfff);

                    if (site + sizeof(uint32_t) > size)
                        continue;

                    *(int32_t*)(image + site) = (int32_t)(target - (site + sizeof(uint32_t)));
                    count++;
                }

                r += block_len;
            }
        }

        q = relocs + reloc_size;
    }

    return count;
}

void apply_function_overrides(uint8_t* image, uint32_t size) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)image;
    IMAGE_NT_HEADERS* nt;
    IMAGE_SECTION_HEADER* sections;
    IMAGE_DATA_DIRECTORY* dd;
    IMAGE_LOAD_CONFIG_DIRECTORY64* cfg;
    uint8_t *dvrt, *p, *end;
    uint32_t dvrt_size;
    fo_caps caps;
    unsigned int total = 0;

    if (size < sizeof(IMAGE_DOS_HEADER) || dos->e_lfanew + sizeof(IMAGE_NT_HEADERS) > size)
        return;

    nt = (IMAGE_NT_HEADERS*)(image + dos->e_lfanew);

    if (nt->OptionalHeader32.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return;

    if (!is_kernel_or_hal(image, size, nt))
        return;

    sections = (IMAGE_SECTION_HEADER*)((uint8_t*)&nt->OptionalHeader64 + nt->FileHeader.SizeOfOptionalHeader);

    for (unsigned int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!memcmp(sections[i].Name, "KSCP    ", 8))
            kernel_scp_reserve_size += (sections[i].VirtualSize + 0xfff) & ~0xfffu;
    }

    dd = &nt->OptionalHeader64.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];

    if (nt->OptionalHeader64.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG || dd->VirtualAddress == 0 ||
        dd->Size < offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, DynamicValueRelocTableSection) + sizeof(uint16_t) ||
        dd->VirtualAddress + dd->Size > size)
        return;

    cfg = (IMAGE_LOAD_CONFIG_DIRECTORY64*)(image + dd->VirtualAddress);

    if (cfg->DynamicValueRelocTableOffset == 0 || cfg->DynamicValueRelocTableSection == 0 ||
        cfg->DynamicValueRelocTableSection > nt->FileHeader.NumberOfSections)
        return;

    {
        uint64_t off = (uint64_t)sections[cfg->DynamicValueRelocTableSection - 1].VirtualAddress +
                       cfg->DynamicValueRelocTableOffset;

        if (off + 8 > size)
            return;

        dvrt = image + off;
    }

    if (((uint32_t*)dvrt)[0] != 1) // version
        return;

    dvrt_size = ((uint32_t*)dvrt)[1];

    if ((uint64_t)(dvrt - image) + 8 + dvrt_size > size)
        return;

    fo_init_caps(caps);

    p = dvrt + 8;
    end = p + dvrt_size;

    while (p + 12 <= end) {
        uint64_t symbol = *(uint64_t*)p;
        uint32_t len = *(uint32_t*)(p + 8);

        if (len > (uint32_t)(end - p - 12))
            break;

        if (symbol == FO_DVRT_FUNCTION_OVERRIDE)
            total += apply(image, size, p + 12, len, caps);

        p += 12 + len;
    }

    if (total != 0)
        print_string("Applied function overrides.\n");
}

#else

void apply_function_overrides(uint8_t* image, uint32_t size) {
    (void)image;
    (void)size;
}

#endif

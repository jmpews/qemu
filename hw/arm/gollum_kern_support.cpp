//
// extern "C" {
// #include "qemu/osdep.h"
// #include "qapi/error.h"
// #include "hw/arm/boot.h"
// #include "exec/address-spaces.h"
// #include "hw/misc/unimp.h"
// #include "sysemu/sysemu.h"
// #include "sysemu/reset.h"
// #include "qemu/error-report.h"
// #include "hw/platform-bus.h"
//
// #include "hw/arm/j273_macos11.h"
//
// #include "hw/arm/exynos4210.h"
// #include "hw/arm/guest-services/general.h"
// }

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>

extern "C" {
#include "qemu/typedefs.h"
#include "exec/hwaddr.h"
#include "exec/memattrs.h"
hwaddr vtop_static(hwaddr va);
MemTxResult address_space_rw(AddressSpace *as, hwaddr addr,
                             MemTxAttrs attrs, void *buf,
                             hwaddr len, bool is_write);
void allocate_ram(MemoryRegion *top, const char *name, hwaddr addr,
                  hwaddr size);
}

#include "mmap_file_util.h"
#include "macho/macho_ctx.h"

struct kernelcache_ctx_t : macho_ctx_t {
  mach_header_t *kernel;

  mach_header_t *kexts[512];
  uint32_t kext_count = 0;

  explicit kernelcache_ctx_t(mach_header_t *header, bool is_runtime_mode = true) : macho_ctx_t(header,
                                                                                               is_runtime_mode) {
    init();
  }

  void init() {
    struct fileset_entry_command *fileset_entry_cmd = 0;
    load_command *curr_cmd;
    curr_cmd = (load_command *) ((uintptr_t) header + sizeof(mach_header_t));
    for (int i = 0; i < header->ncmds; i++) {
      if (curr_cmd->cmd == LC_FILESET_ENTRY) {
        fileset_entry_cmd = (struct fileset_entry_command *) curr_cmd;

        auto kext_header = (mach_header_t *) (fileset_entry_cmd->vmaddr + slide);
        kexts[kext_count++] = kext_header;

        auto kext_name = (char *) ((uintptr_t) fileset_entry_cmd +
                                   (uintptr_t) fileset_entry_cmd->entry_id.offset);
        if (strcmp(kext_name, "com.apple.kernel") == 0) {
          kernel = kext_header;
        }
      }
      curr_cmd = (load_command *) ((uintptr_t) curr_cmd + curr_cmd->cmdsize);
    }
  }
};

typedef uintptr_t pa_t;
typedef struct pmap *pmap_t;
typedef struct task *task_t;

uintptr_t phystokv(uintptr_t pa);

uint64_t phys_read64(uint64_t pa);

void phys_write64(uint64_t pa, uint64_t value);

task_t current_task();

pmap_t get_task_pmap(task_t t);

struct xnu_pre_hack_package_t {
  // --- kernel exported symbols
  __typeof(phystokv) *phystokv;
  __typeof(phys_read64) *phys_read64;
  __typeof(phys_write64) *phys_write64;

  __typeof(current_task) *current_task;
  __typeof(get_task_pmap) *get_task_pmap;
  pmap_t *kernel_pmap_p;

  // --- topOfKernelData area
  uintptr_t gollum_lib_pa;
  uintptr_t gollum_lib_size;
  uintptr_t gollum_init_fn_pa;

  // --- reserved
  uintptr_t reserved[4];
} package;


#define ALIGN_CEIL(address, range) (((uintptr_t)address + (uintptr_t)range - 1) & ~((uintptr_t)range - 1))

#define submask(x) ((1L << ((x) + 1)) - 1)
#define bits(obj, st, fn) (((obj) >> (st)) & submask((fn) - (st)))
#define set_bits(obj, st, fn, bits) obj = (((~(submask(fn - st) << st)) & obj) | (bits << st))


uint32_t encode_adr(uint8_t reg_id, uintptr_t src, uintptr_t dst) {
  uint32_t adr_inst = 0x10000000 | reg_id;
  uint64_t diff = dst - src;
  uint64_t immlo = bits(diff, 0, 1);
  uint64_t immhi = bits(diff, 2, 20);
  set_bits(adr_inst, 29, 30, immlo);
  set_bits(adr_inst, 5, 23, immhi);
  return adr_inst;
}

uint32_t encode_bl(uintptr_t source, uintptr_t target) {
  uint32_t bl_inst = 0x94000000;
  uint64_t diff_imm = (target - source) >> 2;
  bl_inst |= bits(diff_imm, 0, 25);
  return bl_inst;
}

uint32_t encode_b(uintptr_t source, uintptr_t target) {
  uint32_t b_inst = 0x14000000;
  uint64_t diff_imm = (target - source) >> 2;
  b_inst |= bits(diff_imm, 0, 25);
  return b_inst;
}

void encod_insn_seq(AddressSpace *nsas, hwaddr shellcode_area, int *in_out_offset, uint32_t *insn_seq,
                    uint32_t num_insns) {
  address_space_rw(nsas, vtop_static(shellcode_area + *in_out_offset),
                   MEMTXATTRS_UNSPECIFIED, (uint8_t *) insn_seq,
                   num_insns * sizeof(uint32_t), 1);
  *in_out_offset += num_insns * sizeof(uint32_t);
}

extern "C" uintptr_t
craft_shellcode(AddressSpace *nsas, MemoryRegion *mem, uintptr_t *in_out_curr_pa, uintptr_t hook_addr,
                uintptr_t kernelcache_base) {
  char *gollum_lib_path = "/usr/local/workspace/project.wrk/ResearchWorkspace/cmake-build-macos-silicon-kern/gollum_kern/libgollum_kern.dylib";
  gollum_lib_path = "/usr/local/Workspace/Project.wrk/ResearchWorkspace/cmake-build-macos-arm64-kern/gollum_kern/libgollum_kern.dylib";

  MmapFileManager gollum_lib(gollum_lib_path);
  gollum_lib.map();
  macho_ctx_t gollum_lib_ctx((mach_header_t *) gollum_lib.mmap_buffer);
  auto gollum_init_fn_offset = gollum_lib_ctx.iterate_exported_symbol("_gollum_init", 0);
  fprintf(stderr, "gollum_init_fn_offset: %p\n", gollum_init_fn_offset);

  size_t alloc_size = ALIGN_CEIL(gollum_lib.mmap_buffer_size, 0x4000);
  allocate_ram(mem, "gollum_lib", *in_out_curr_pa, alloc_size);
  address_space_rw(nsas, *in_out_curr_pa, MEMTXATTRS_UNSPECIFIED, gollum_lib.mmap_buffer, gollum_lib.mmap_buffer_size,
                   true);
  auto gollum_lib_pa = *in_out_curr_pa;
  *in_out_curr_pa += alloc_size;
  package.gollum_lib_pa = gollum_lib_pa;
  package.gollum_lib_size = gollum_lib.mmap_buffer_size;
  package.gollum_init_fn_pa = gollum_lib_pa + gollum_init_fn_offset;
  fprintf(stderr, "gollum_lib_pa: %p, gollum_lib_size: %p, gollum_init_fn_pa: %p\n",
          package.gollum_lib_pa, package.gollum_lib_size, package.gollum_init_fn_pa);

  auto gPongoHandoff_off = gollum_lib_ctx.symbol_resolve("_gPongoHandoff") - (uintptr_t) gollum_lib_ctx.header;
  uintptr_t gPongoHandoff_pa = gollum_lib_pa + gPongoHandoff_off;
  address_space_rw(nsas, gPongoHandoff_pa, MEMTXATTRS_UNSPECIFIED, (uint8_t *) &kernelcache_base, 8, 1);
  fprintf(stderr, "gPongoHandoff_pa: %p\n", gPongoHandoff_pa);

  MmapFileManager kc("/Users/jmpews/Downloads/20C69/kernelcache.release.j273.out");
  kc.map();
  kernelcache_ctx_t kc_ctx((mach_header_t *) kc.mmap_buffer, false);

  macho_ctx_t kernel_ctx(kc_ctx.kernel, false, kc_ctx.header);
  package.phystokv = (__typeof(package.phystokv)) kernel_ctx.symbol_resolve("_ml_static_ptovirt");
  package.phys_read64 = (__typeof(package.phys_read64)) kernel_ctx.symbol_resolve("_ml_phys_read_double_64");
  package.phys_write64 = (__typeof(package.phys_write64)) kernel_ctx.symbol_resolve("_ml_phys_write_double_64");
  package.current_task = (__typeof(package.current_task)) kernel_ctx.symbol_resolve("_current_task");
  package.get_task_pmap = (__typeof(package.get_task_pmap)) kernel_ctx.symbol_resolve("_get_task_pmap");
  package.kernel_pmap_p = (__typeof(package.kernel_pmap_p)) kernel_ctx.symbol_resolve("_kernel_pmap");

  const uintptr_t shellcode_area = 0xfffffe0007ac5784;

  int offset = 0;
  uintptr_t params_addr = shellcode_area + offset;
  encod_insn_seq(nsas, shellcode_area, &offset, (uint32_t *) &package, sizeof(package) / 4);

  uintptr_t shellcode_start_addr = shellcode_area + offset;
  uint32_t addr_x0_params = encode_adr(0, shellcode_start_addr, params_addr);
  encod_insn_seq(nsas, shellcode_area, &offset, &addr_x0_params, 1);
  fprintf(stderr, "shellcode_start_addr: %p: addr_x0_params: %p\n", shellcode_start_addr, (void *) addr_x0_params);

  auto xnu_pre_hack_shellcode_start_off = gollum_lib_ctx.iterate_exported_symbol("_xnu_pre_hack_shellcode", 0);
  auto xnu_pre_hack_shellcode_end_off = gollum_lib_ctx.iterate_exported_symbol("_xnu_pre_hack_shellcode_end", 0);
  encod_insn_seq(nsas, shellcode_area, &offset,
                 (uint32_t *) (offset, gollum_lib.mmap_buffer + xnu_pre_hack_shellcode_start_off),
                 (xnu_pre_hack_shellcode_end_off - xnu_pre_hack_shellcode_start_off) / 4);
  uintptr_t shellcode_end_addr = shellcode_area + offset;

  fprintf(stderr, "xnu_pre_hack_shellcode_start_off: %p, xnu_pre_hack_shellcode_end_off: %p\n",
          xnu_pre_hack_shellcode_start_off, xnu_pre_hack_shellcode_end_off);


  uint32_t b_shellcode = encode_b(hook_addr, shellcode_start_addr);
  address_space_rw(nsas, vtop_static(hook_addr), MEMTXATTRS_UNSPECIFIED, (uint8_t *) &b_shellcode, 4, 1);

  uint32_t b_back = encode_b(shellcode_area - 4, hook_addr + 4);
  address_space_rw(nsas, vtop_static(shellcode_area - 4), MEMTXATTRS_UNSPECIFIED, (uint8_t *) &b_back, 4, 1);

  return shellcode_start_addr;
}
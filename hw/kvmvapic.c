/*
 * TPR optimization for 32-bit Windows guests (XP and Server 2003)
 *
 * Copyright (C) 2007-2008 Qumranet Technologies
 * Copyright (C) 2012      Jan Kiszka, Siemens AG
 *
 * This work is licensed under the terms of the GNU GPL version 2, or
 * (at your option) any later version. See the COPYING file in the
 * top-level directory.
 */
#include "sysemu.h"
#include "cpus.h"
#include "kvm.h"
#include "apic_internal.h"

#define APIC_DEFAULT_ADDRESS    0xfee00000

#define VAPIC_IO_PORT           0x7e

#define VAPIC_CPU_SHIFT         7

#define ROM_BLOCK_SIZE          512
#define ROM_BLOCK_MASK          (~(ROM_BLOCK_SIZE - 1))

typedef enum VAPICMode {
    VAPIC_INACTIVE = 0,
    VAPIC_ACTIVE   = 1,
    VAPIC_STANDBY  = 2,
} VAPICMode;

typedef struct VAPICHandlers {
    uint32_t set_tpr;
    uint32_t set_tpr_eax;
    uint32_t get_tpr[8];
    uint32_t get_tpr_stack;
} QEMU_PACKED VAPICHandlers;

typedef struct GuestROMState {
    char signature[8];
    uint32_t vaddr;
    uint32_t fixup_start;
    uint32_t fixup_end;
    uint32_t vapic_vaddr;
    uint32_t vapic_size;
    uint32_t vcpu_shift;
    uint32_t real_tpr_addr;
    VAPICHandlers up;
    VAPICHandlers mp;
} QEMU_PACKED GuestROMState;

typedef struct VAPICROMState {
    SysBusDevice busdev;
    MemoryRegion io;
    MemoryRegion rom;
    uint32_t state;
    uint32_t rom_state_paddr;
    uint32_t rom_state_vaddr;
    uint32_t vapic_paddr;
    uint32_t real_tpr_addr;
    GuestROMState rom_state;
    size_t rom_size;
    bool rom_mapped_writable;
} VAPICROMState;

#define TPR_INSTR_ABS_MODRM             0x1
#define TPR_INSTR_MATCH_MODRM_REG       0x2

typedef struct TPRInstruction {
    uint8_t opcode;
    uint8_t modrm_reg;
    unsigned int flags;
    TPRAccess access;
    size_t length;
    off_t addr_offset;
} TPRInstruction;

/* must be sorted by length, shortest first */
static const TPRInstruction tpr_instr[] = {
    { /* mov abs to eax */
        .opcode = 0xa1,
        .access = TPR_ACCESS_READ,
        .length = 5,
        .addr_offset = 1,
    },
    { /* mov eax to abs */
        .opcode = 0xa3,
        .access = TPR_ACCESS_WRITE,
        .length = 5,
        .addr_offset = 1,
    },
    { /* mov r32 to r/m32 */
        .opcode = 0x89,
        .flags = TPR_INSTR_ABS_MODRM,
        .access = TPR_ACCESS_WRITE,
        .length = 6,
        .addr_offset = 2,
    },
    { /* mov r/m32 to r32 */
        .opcode = 0x8b,
        .flags = TPR_INSTR_ABS_MODRM,
        .access = TPR_ACCESS_READ,
        .length = 6,
        .addr_offset = 2,
    },
    { /* push r/m32 */
        .opcode = 0xff,
        .modrm_reg = 6,
        .flags = TPR_INSTR_ABS_MODRM | TPR_INSTR_MATCH_MODRM_REG,
        .access = TPR_ACCESS_READ,
        .length = 6,
        .addr_offset = 2,
    },
    { /* mov imm32, r/m32 (c7/0) */
        .opcode = 0xc7,
        .modrm_reg = 0,
        .flags = TPR_INSTR_ABS_MODRM | TPR_INSTR_MATCH_MODRM_REG,
        .access = TPR_ACCESS_WRITE,
        .length = 10,
        .addr_offset = 2,
    },
};

static void read_guest_rom_state(VAPICROMState *s)
{
    cpu_physical_memory_rw(s->rom_state_paddr, (void *)&s->rom_state,
                           sizeof(GuestROMState), 0);
}

static void write_guest_rom_state(VAPICROMState *s)
{
    cpu_physical_memory_rw(s->rom_state_paddr, (void *)&s->rom_state,
                           sizeof(GuestROMState), 1);
}

static void update_guest_rom_state(VAPICROMState *s)
{
    read_guest_rom_state(s);

    s->rom_state.real_tpr_addr = cpu_to_le32(s->real_tpr_addr);
    s->rom_state.vcpu_shift = cpu_to_le32(VAPIC_CPU_SHIFT);

    write_guest_rom_state(s);
}

static int find_real_tpr_addr(VAPICROMState *s, CPUX86State *env)
{
    hwaddr paddr;
    target_ulong addr;

    if (s->state == VAPIC_ACTIVE) {
        return 0;
    }
    /*
     * If there is no prior TPR access instruction we could analyze (which is
     * the case after resume from hibernation), we need to scan the possible
     * virtual address space for the APIC mapping.
     */
    for (addr = 0xfffff000; addr >= 0x80000000; addr -= TARGET_PAGE_SIZE) {
        paddr = cpu_get_phys_page_debug(env, addr);
        if (paddr != APIC_DEFAULT_ADDRESS) {
            continue;
        }
        s->real_tpr_addr = addr + 0x80;
        update_guest_rom_state(s);
        return 0;
    }
    return -1;
}

static uint8_t modrm_reg(uint8_t modrm)
{
    return (modrm >> 3) & 7;
}

static bool is_abs_modrm(uint8_t modrm)
{
    return (modrm & 0xc7) == 0x05;
}

static bool opcode_matches(uint8_t *opcode, const TPRInstruction *instr)
{
    return opcode[0] == instr->opcode &&
        (!(instr->flags & TPR_INSTR_ABS_MODRM) || is_abs_modrm(opcode[1])) &&
        (!(instr->flags & TPR_INSTR_MATCH_MODRM_REG) ||
         modrm_reg(opcode[1]) == instr->modrm_reg);
}

static int evaluate_tpr_instruction(VAPICROMState *s, CPUX86State *env,
                                    target_ulong *pip, TPRAccess access)
{
    const TPRInstruction *instr;
    target_ulong ip = *pip;
    uint8_t opcode[2];
    uint32_t real_tpr_addr;
    int i;

    if ((ip & 0xf0000000ULL) != 0x80000000ULL &&
        (ip & 0xf0000000ULL) != 0xe0000000ULL) {
        return -1;
    }

    /*
     * Early Windows 2003 SMP initialization contains a
     *
     *   mov imm32, r/m32
     *
     * instruction that is patched by TPR optimization. The problem is that
     * RSP, used by the patched instruction, is zero, so the guest gets a
     * double fault and dies.
     */
    if (env->regs[R_ESP] == 0) {
        return -1;
    }

    if (kvm_enabled() && !kvm_irqchip_in_kernel()) {
        /*
         * KVM without kernel-based TPR access reporting will pass an IP that
         * points after the accessing instruction. So we need to look backward
         * to find the reason.
         */
        for (i = 0; i < ARRAY_SIZE(tpr_instr); i++) {
            instr = &tpr_instr[i];
            if (instr->access != access) {
                continue;
            }
            if (cpu_memory_rw_debug(env, ip - instr->length, opcode,
                                    sizeof(opcode), 0) < 0) {
                return -1;
            }
            if (opcode_matches(opcode, instr)) {
                ip -= instr->length;
                goto instruction_ok;
            }
        }
        return -1;
    } else {
        if (cpu_memory_rw_debug(env, ip, opcode, sizeof(opcode), 0) < 0) {
            return -1;
        }
        for (i = 0; i < ARRAY_SIZE(tpr_instr); i++) {
            instr = &tpr_instr[i];
            if (opcode_matches(opcode, instr)) {
                goto instruction_ok;
            }
        }
        return -1;
    }

instruction_ok:
    /*
     * Grab the virtual TPR address from the instruction
     * and update the cached values.
     */
    if (cpu_memory_rw_debug(env, ip + instr->addr_offset,
                            (void *)&real_tpr_addr,
                            sizeof(real_tpr_addr), 0) < 0) {
        return -1;
    }
    real_tpr_addr = le32_to_cpu(real_tpr_addr);
    if ((real_tpr_addr & 0xfff) != 0x80) {
        return -1;
    }
    s->real_tpr_addr = real_tpr_addr;
    update_guest_rom_state(s);

    *pip = ip;
    return 0;
}

static int update_rom_mapping(VAPICROMState *s, CPUX86State *env, target_ulong ip)
{
    hwaddr paddr;
    uint32_t rom_state_vaddr;
    uint32_t pos, patch, offset;

    /* nothing to do if already activated */
    if (s->state == VAPIC_ACTIVE) {
        return 0;
    }

    /* bail out if ROM init code was not executed (missing ROM?) */
    if (s->state == VAPIC_INACTIVE) {
        return -1;
    }

    /* find out virtual address of the ROM */
    rom_state_vaddr = s->rom_state_paddr + (ip & 0xf0000000);
    paddr = cpu_get_phys_page_debug(env, rom_state_vaddr);
    if (paddr == -1) {
        return -1;
    }
    paddr += rom_state_vaddr & ~TARGET_PAGE_MASK;
    if (paddr != s->rom_state_paddr) {
        return -1;
    }
    read_guest_rom_state(s);
    if (memcmp(s->rom_state.signature, "kvm aPiC", 8) != 0) {
        return -1;
    }
    s->rom_state_vaddr = rom_state_vaddr;

    /* fixup addresses in ROM if needed */
    if (rom_state_vaddr == le32_to_cpu(s->rom_state.vaddr)) {
        return 0;
    }
    for (pos = le32_to_cpu(s->rom_state.fixup_start);
         pos < le32_to_cpu(s->rom_state.fixup_end);
         pos += 4) {
        cpu_physical_memory_rw(paddr + pos - s->rom_state.vaddr,
                               (void *)&offset, sizeof(offset), 0);
        offset = le32_to_cpu(offset);
        cpu_physical_memory_rw(paddr + offset, (void *)&patch,
                               sizeof(patch), 0);
        patch = le32_to_cpu(patch);
        patch += rom_state_vaddr - le32_to_cpu(s->rom_state.vaddr);
        patch = cpu_to_le32(patch);
        cpu_physical_memory_rw(paddr + offset, (void *)&patch,
                               sizeof(patch), 1);
    }
    read_guest_rom_state(s);
    s->vapic_paddr = paddr + le32_to_cpu(s->rom_state.vapic_vaddr) -
        le32_to_cpu(s->rom_state.vaddr);

    return 0;
}

/*
 * Tries to read the unique processor number from the Kernel Processor Control
 * Region (KPCR) of 32-bit Windows XP and Server 2003. Returns -1 if the KPCR
 * cannot be accessed or is considered invalid. This also ensures that we are
 * not patching the wrong guest.
 */
static int get_kpcr_number(CPUX86State *env)
{
    struct kpcr {
        uint8_t  fill1[0x1c];
        uint32_t self;
        uint8_t  fill2[0x31];
        uint8_t  number;
    } QEMU_PACKED kpcr;

    if (cpu_memory_rw_debug(env, env->segs[R_FS].base,
                            (void *)&kpcr, sizeof(kpcr), 0) < 0 ||
        kpcr.self != env->segs[R_FS].base) {
        return -1;
    }
    return kpcr.number;
}

static int vapic_enable(VAPICROMState *s, CPUX86State *env)
{
    int cpu_number = get_kpcr_number(env);
    hwaddr vapic_paddr;
    static const uint8_t enabled = 1;

    if (cpu_number < 0) {
        return -1;
    }
    vapic_paddr = s->vapic_paddr +
        (((hwaddr)cpu_number) << VAPIC_CPU_SHIFT);
    cpu_physical_memory_rw(vapic_paddr + offsetof(VAPICState, enabled),
                           (void *)&enabled, sizeof(enabled), 1);
    apic_enable_vapic(env->apic_state, vapic_paddr);

    s->state = VAPIC_ACTIVE;

    return 0;
}

static void patch_byte(CPUX86State *env, target_ulong addr, uint8_t byte)
{
    cpu_memory_rw_debug(env, addr, &byte, 1, 1);
}

static void patch_call(VAPICROMState *s, CPUX86State *env, target_ulong ip,
                       uint32_t target)
{
    uint32_t offset;

    offset = cpu_to_le32(target - ip - 5);
    patch_byte(env, ip, 0xe8); /* call near */
    cpu_memory_rw_debug(env, ip + 1, (void *)&offset, sizeof(offset), 1);
}

static void patch_instruction(VAPICROMState *s, CPUX86State *env, target_ulong ip)
{
    hwaddr paddr;
    VAPICHandlers *handlers;
    uint8_t opcode[2];
    uint32_t imm32;

    if (smp_cpus == 1) {
        handlers = &s->rom_state.up;
    } else {
        handlers = &s->rom_state.mp;
    }

    pause_all_vcpus();

    cpu_memory_rw_debug(env, ip, opcode, sizeof(opcode), 0);

    switch (opcode[0]) {
    case 0x89: /* mov r32 to r/m32 */
        patch_byte(env, ip, 0x50 + modrm_reg(opcode[1]));  /* push reg */
        patch_call(s, env, ip + 1, handlers->set_tpr);
        break;
    case 0x8b: /* mov r/m32 to r32 */
        patch_byte(env, ip, 0x90);
        patch_call(s, env, ip + 1, handlers->get_tpr[modrm_reg(opcode[1])]);
        break;
    case 0xa1: /* mov abs to eax */
        patch_call(s, env, ip, handlers->get_tpr[0]);
        break;
    case 0xa3: /* mov eax to abs */
        patch_call(s, env, ip, handlers->set_tpr_eax);
        break;
    case 0xc7: /* mov imm32, r/m32 (c7/0) */
        patch_byte(env, ip, 0x68);  /* push imm32 */
        cpu_memory_rw_debug(env, ip + 6, (void *)&imm32, sizeof(imm32), 0);
        cpu_memory_rw_debug(env, ip + 1, (void *)&imm32, sizeof(imm32), 1);
        patch_call(s, env, ip + 5, handlers->set_tpr);
        break;
    case 0xff: /* push r/m32 */
        patch_byte(env, ip, 0x50); /* push eax */
        patch_call(s, env, ip + 1, handlers->get_tpr_stack);
        break;
    default:
        abort();
    }

    resume_all_vcpus();

    paddr = cpu_get_phys_page_debug(env, ip);
    paddr += ip & ~TARGET_PAGE_MASK;
    tb_invalidate_phys_page_range(paddr, paddr + 1, 1);
}

void vapic_report_tpr_access(DeviceState *dev, void *cpu, target_ulong ip,
                             TPRAccess access)
{
    VAPICROMState *s = DO_UPCAST(VAPICROMState, busdev.qdev, dev);
    CPUX86State *env = cpu;

    cpu_synchronize_state(env);

    if (evaluate_tpr_instruction(s, env, &ip, access) < 0) {
        if (s->state == VAPIC_ACTIVE) {
            vapic_enable(s, env);
        }
        return;
    }
    if (update_rom_mapping(s, env, ip) < 0) {
        return;
    }
    if (vapic_enable(s, env) < 0) {
        return;
    }
    patch_instruction(s, env, ip);
}

typedef struct VAPICEnableTPRReporting {
    DeviceState *apic;
    bool enable;
} VAPICEnableTPRReporting;

static void vapic_do_enable_tpr_reporting(void *data)
{
    VAPICEnableTPRReporting *info = data;

    apic_enable_tpr_access_reporting(info->apic, info->enable);
}

static void vapic_enable_tpr_reporting(bool enable)
{
    VAPICEnableTPRReporting info = {
        .enable = enable,
    };
    X86CPU *cpu;
    CPUX86State *env;

    for (env = first_cpu; env != NULL; env = env->next_cpu) {
        cpu = x86_env_get_cpu(env);
        info.apic = env->apic_state;
        run_on_cpu(CPU(cpu), vapic_do_enable_tpr_reporting, &info);
    }
}

static void vapic_reset(DeviceState *dev)
{
    VAPICROMState *s = DO_UPCAST(VAPICROMState, busdev.qdev, dev);

    if (s->state == VAPIC_ACTIVE) {
        s->state = VAPIC_STANDBY;
    }
    vapic_enable_tpr_reporting(false);
}

/*
 * Set the IRQ polling hypercalls to the supported variant:
 *  - vmcall if using KVM in-kernel irqchip
 *  - 32-bit VAPIC port write otherwise
 */
static int patch_hypercalls(VAPICROMState *s)
{
    hwaddr rom_paddr = s->rom_state_paddr & ROM_BLOCK_MASK;
    static const uint8_t vmcall_pattern[] = { /* vmcall */
        0xb8, 0x1, 0, 0, 0, 0xf, 0x1, 0xc1
    };
    static const uint8_t outl_pattern[] = { /* nop; outl %eax,0x7e */
        0xb8, 0x1, 0, 0, 0, 0x90, 0xe7, 0x7e
    };
    uint8_t alternates[2];
    const uint8_t *pattern;
    const uint8_t *patch;
    int patches = 0;
    off_t pos;
    uint8_t *rom;

    rom = g_malloc(s->rom_size);
    cpu_physical_memory_rw(rom_paddr, rom, s->rom_size, 0);

    for (pos = 0; pos < s->rom_size - sizeof(vmcall_pattern); pos++) {
        if (kvm_irqchip_in_kernel()) {
            pattern = outl_pattern;
            alternates[0] = outl_pattern[7];
            alternates[1] = outl_pattern[7];
            patch = &vmcall_pattern[5];
        } else {
            pattern = vmcall_pattern;
            alternates[0] = vmcall_pattern[7];
            alternates[1] = 0xd9; /* AMD's VMMCALL */
            patch = &outl_pattern[5];
        }
        if (memcmp(rom + pos, pattern, 7) == 0 &&
            (rom[pos + 7] == alternates[0] || rom[pos + 7] == alternates[1])) {
            cpu_physical_memory_rw(rom_paddr + pos + 5, (uint8_t *)patch,
                                   3, 1);
            /*
             * Don't flush the tb here. Under ordinary conditions, the patched
             * calls are miles away from the current IP. Under malicious
             * conditions, the guest could trick us to crash.
             */
        }
    }

    g_free(rom);

    if (patches != 0 && patches != 2) {
        return -1;
    }

    return 0;
}

/*
 * For TCG mode or the time KVM honors read-only memory regions, we need to
 * enable write access to the option ROM so that variables can be updated by
 * the guest.
 */
static void vapic_map_rom_writable(VAPICROMState *s)
{
    hwaddr rom_paddr = s->rom_state_paddr & ROM_BLOCK_MASK;
    MemoryRegionSection section;
    MemoryRegion *as;
    size_t rom_size;
    uint8_t *ram;

    as = sysbus_address_space(&s->busdev);

    if (s->rom_mapped_writable) {
        memory_region_del_subregion(as, &s->rom);
        memory_region_destroy(&s->rom);
    }

    /* grab RAM memory region (region @rom_paddr may still be pc.rom) */
    section = memory_region_find(as, 0, 1);

    /* read ROM size from RAM region */
    ram = memory_region_get_ram_ptr(section.mr);
    rom_size = ram[rom_paddr + 2] * ROM_BLOCK_SIZE;
    s->rom_size = rom_size;

    /* We need to round to avoid creating subpages
     * from which we cannot run code. */
    rom_size += rom_paddr & ~TARGET_PAGE_MASK;
    rom_paddr &= TARGET_PAGE_MASK;
    rom_size = TARGET_PAGE_ALIGN(rom_size);

    memory_region_init_alias(&s->rom, "kvmvapic-rom", section.mr, rom_paddr,
                             rom_size);
    memory_region_add_subregion_overlap(as, rom_paddr, &s->rom, 1000);
    s->rom_mapped_writable = true;
}

static int vapic_prepare(VAPICROMState *s)
{
    vapic_map_rom_writable(s);

    if (patch_hypercalls(s) < 0) {
        return -1;
    }

    vapic_enable_tpr_reporting(true);

    return 0;
}

static void vapic_write(void *opaque, hwaddr addr, uint64_t data,
                        unsigned int size)
{
    CPUX86State *env = cpu_single_env;
    hwaddr rom_paddr;
    VAPICROMState *s = opaque;

    cpu_synchronize_state(env);

    /*
     * The VAPIC supports two PIO-based hypercalls, both via port 0x7E.
     *  o 16-bit write access:
     *    Reports the option ROM initialization to the hypervisor. Written
     *    value is the offset of the state structure in the ROM.
     *  o 8-bit write access:
     *    Reactivates the VAPIC after a guest hibernation, i.e. after the
     *    option ROM content has been re-initialized by a guest power cycle.
     *  o 32-bit write access:
     *    Poll for pending IRQs, considering the current VAPIC state.
     */
    switch (size) {
    case 2:
        if (s->state == VAPIC_INACTIVE) {
            rom_paddr = (env->segs[R_CS].base + env->eip) & ROM_BLOCK_MASK;
            s->rom_state_paddr = rom_paddr + data;

            s->state = VAPIC_STANDBY;
        }
        if (vapic_prepare(s) < 0) {
            s->state = VAPIC_INACTIVE;
            break;
        }
        break;
    case 1:
        if (kvm_enabled()) {
            /*
             * Disable triggering instruction in ROM by writing a NOP.
             *
             * We cannot do this in TCG mode as the reported IP is not
             * accurate.
             */
            pause_all_vcpus();
            patch_byte(env, env->eip - 2, 0x66);
            patch_byte(env, env->eip - 1, 0x90);
            resume_all_vcpus();
        }

        if (s->state == VAPIC_ACTIVE) {
            break;
        }
        if (update_rom_mapping(s, env, env->eip) < 0) {
            break;
        }
        if (find_real_tpr_addr(s, env) < 0) {
            break;
        }
        vapic_enable(s, env);
        break;
    default:
    case 4:
        if (!kvm_irqchip_in_kernel()) {
            apic_poll_irq(env->apic_state);
        }
        break;
    }
}

static const MemoryRegionOps vapic_ops = {
    .write = vapic_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static int vapic_init(SysBusDevice *dev)
{
    VAPICROMState *s = FROM_SYSBUS(VAPICROMState, dev);

    memory_region_init_io(&s->io, &vapic_ops, s, "kvmvapic", 2);
    sysbus_add_io(dev, VAPIC_IO_PORT, &s->io);
    sysbus_init_ioports(dev, VAPIC_IO_PORT, 2);

    option_rom[nb_option_roms].name = "kvmvapic.bin";
    option_rom[nb_option_roms].bootindex = -1;
    nb_option_roms++;

    return 0;
}

static void do_vapic_enable(void *data)
{
    VAPICROMState *s = data;

    vapic_enable(s, first_cpu);
}

static int vapic_post_load(void *opaque, int version_id)
{
    VAPICROMState *s = opaque;
    uint8_t *zero;

    /*
     * The old implementation of qemu-kvm did not provide the state
     * VAPIC_STANDBY. Reconstruct it.
     */
    if (s->state == VAPIC_INACTIVE && s->rom_state_paddr != 0) {
        s->state = VAPIC_STANDBY;
    }

    if (s->state != VAPIC_INACTIVE) {
        if (vapic_prepare(s) < 0) {
            return -1;
        }
    }
    if (s->state == VAPIC_ACTIVE) {
        if (smp_cpus == 1) {
            run_on_cpu(ENV_GET_CPU(first_cpu), do_vapic_enable, s);
        } else {
            zero = g_malloc0(s->rom_state.vapic_size);
            cpu_physical_memory_rw(s->vapic_paddr, zero,
                                   s->rom_state.vapic_size, 1);
            g_free(zero);
        }
    }

    return 0;
}

static const VMStateDescription vmstate_handlers = {
    .name = "kvmvapic-handlers",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(set_tpr, VAPICHandlers),
        VMSTATE_UINT32(set_tpr_eax, VAPICHandlers),
        VMSTATE_UINT32_ARRAY(get_tpr, VAPICHandlers, 8),
        VMSTATE_UINT32(get_tpr_stack, VAPICHandlers),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_guest_rom = {
    .name = "kvmvapic-guest-rom",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UNUSED(8),     /* signature */
        VMSTATE_UINT32(vaddr, GuestROMState),
        VMSTATE_UINT32(fixup_start, GuestROMState),
        VMSTATE_UINT32(fixup_end, GuestROMState),
        VMSTATE_UINT32(vapic_vaddr, GuestROMState),
        VMSTATE_UINT32(vapic_size, GuestROMState),
        VMSTATE_UINT32(vcpu_shift, GuestROMState),
        VMSTATE_UINT32(real_tpr_addr, GuestROMState),
        VMSTATE_STRUCT(up, GuestROMState, 0, vmstate_handlers, VAPICHandlers),
        VMSTATE_STRUCT(mp, GuestROMState, 0, vmstate_handlers, VAPICHandlers),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_vapic = {
    .name = "kvm-tpr-opt",      /* compatible with qemu-kvm VAPIC */
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .post_load = vapic_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_STRUCT(rom_state, VAPICROMState, 0, vmstate_guest_rom,
                       GuestROMState),
        VMSTATE_UINT32(state, VAPICROMState),
        VMSTATE_UINT32(real_tpr_addr, VAPICROMState),
        VMSTATE_UINT32(rom_state_vaddr, VAPICROMState),
        VMSTATE_UINT32(vapic_paddr, VAPICROMState),
        VMSTATE_UINT32(rom_state_paddr, VAPICROMState),
        VMSTATE_END_OF_LIST()
    }
};

static void vapic_class_init(ObjectClass *klass, void *data)
{
    SysBusDeviceClass *sc = SYS_BUS_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->no_user = 1;
    dc->reset   = vapic_reset;
    dc->vmsd    = &vmstate_vapic;
    sc->init    = vapic_init;
}

static TypeInfo vapic_type = {
    .name          = "kvmvapic",
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VAPICROMState),
    .class_init    = vapic_class_init,
};

static void vapic_register(void)
{
    type_register_static(&vapic_type);
}

type_init(vapic_register);

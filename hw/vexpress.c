/*
 * ARM Versatile Express emulation.
 *
 * Copyright (c) 2010 - 2011 B Labs Ltd.
 * Copyright (c) 2011 Linaro Limited
 * Written by Bahadir Balban, Amit Mahajan, Peter Maydell
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 *  Contributions after 2012-01-13 are licensed under the terms of the
 *  GNU GPL, version 2 or (at your option) any later version.
 */

#include "sysbus.h"
#include "arm-misc.h"
#include "primecell.h"
#include "devices.h"
#include "net.h"
#include "sysemu.h"
#include "boards.h"
#include "exec-memory.h"

#define VEXPRESS_BOARD_ID 0x8e0

static struct arm_boot_info vexpress_binfo;

/* Address maps for peripherals:
 * the Versatile Express motherboard has two possible maps,
 * the "legacy" one (used for A9) and the "Cortex-A Series"
 * map (used for newer cores).
 * Individual daughterboards can also have different maps for
 * their peripherals.
 */

enum {
    VE_SYSREGS,
    VE_SP810,
    VE_SERIALPCI,
    VE_PL041,
    VE_MMCI,
    VE_KMI0,
    VE_KMI1,
    VE_UART0,
    VE_UART1,
    VE_UART2,
    VE_UART3,
    VE_WDT,
    VE_TIMER01,
    VE_TIMER23,
    VE_SERIALDVI,
    VE_RTC,
    VE_COMPACTFLASH,
    VE_CLCD,
    VE_NORFLASH0,
    VE_NORFLASH0ALIAS,
    VE_NORFLASH1,
    VE_SRAM,
    VE_VIDEORAM,
    VE_ETHERNET,
    VE_USB,
    VE_DAPROM,
};

static target_phys_addr_t motherboard_legacy_map[] = {
    /* CS7: 0x10000000 .. 0x10020000 */
    [VE_SYSREGS] = 0x10000000,
    [VE_SP810] = 0x10001000,
    [VE_SERIALPCI] = 0x10002000,
    [VE_PL041] = 0x10004000,
    [VE_MMCI] = 0x10005000,
    [VE_KMI0] = 0x10006000,
    [VE_KMI1] = 0x10007000,
    [VE_UART0] = 0x10009000,
    [VE_UART1] = 0x1000a000,
    [VE_UART2] = 0x1000b000,
    [VE_UART3] = 0x1000c000,
    [VE_WDT] = 0x1000f000,
    [VE_TIMER01] = 0x10011000,
    [VE_TIMER23] = 0x10012000,
    [VE_SERIALDVI] = 0x10016000,
    [VE_RTC] = 0x10017000,
    [VE_COMPACTFLASH] = 0x1001a000,
    [VE_CLCD] = 0x1001f000,
    /* CS0: 0x40000000 .. 0x44000000 */
    [VE_NORFLASH0] = 0x40000000,
    /* CS1: 0x44000000 .. 0x48000000 */
    [VE_NORFLASH1] = 0x44000000,
    /* CS2: 0x48000000 .. 0x4a000000 */
    [VE_SRAM] = 0x48000000,
    /* CS3: 0x4c000000 .. 0x50000000 */
    [VE_VIDEORAM] = 0x4c000000,
    [VE_ETHERNET] = 0x4e000000,
    [VE_USB] = 0x4f000000,
};

static void vexpress_a9_init(ram_addr_t ram_size,
                     const char *boot_device,
                     const char *kernel_filename, const char *kernel_cmdline,
                     const char *initrd_filename, const char *cpu_model)
{
    CPUState *env = NULL;
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *ram = g_new(MemoryRegion, 1);
    MemoryRegion *lowram = g_new(MemoryRegion, 1);
    MemoryRegion *vram = g_new(MemoryRegion, 1);
    MemoryRegion *sram = g_new(MemoryRegion, 1);
    DeviceState *dev, *sysctl, *pl041;
    SysBusDevice *busdev;
    qemu_irq *irqp;
    qemu_irq pic[64];
    int n;
    qemu_irq cpu_irq[4];
    uint32_t proc_id;
    uint32_t sys_id;
    ram_addr_t low_ram_size, vram_size, sram_size;
    target_phys_addr_t *map = motherboard_legacy_map;

    if (!cpu_model) {
        cpu_model = "cortex-a9";
    }

    for (n = 0; n < smp_cpus; n++) {
        env = cpu_init(cpu_model);
        if (!env) {
            fprintf(stderr, "Unable to find CPU definition\n");
            exit(1);
        }
        irqp = arm_pic_init_cpu(env);
        cpu_irq[n] = irqp[ARM_PIC_CPU_IRQ];
    }

    if (ram_size > 0x40000000) {
        /* 1GB is the maximum the address space permits */
        fprintf(stderr, "vexpress: cannot model more than 1GB RAM\n");
        exit(1);
    }

    memory_region_init_ram(ram, "vexpress.highmem", ram_size);
    vmstate_register_ram_global(ram);
    low_ram_size = ram_size;
    if (low_ram_size > 0x4000000) {
        low_ram_size = 0x4000000;
    }
    /* RAM is from 0x60000000 upwards. The bottom 64MB of the
     * address space should in theory be remappable to various
     * things including ROM or RAM; we always map the RAM there.
     */
    memory_region_init_alias(lowram, "vexpress.lowmem", ram, 0, low_ram_size);
    memory_region_add_subregion(sysmem, 0x0, lowram);
    memory_region_add_subregion(sysmem, 0x60000000, ram);

    /* 0x1e000000 A9MPCore (SCU) private memory region */
    dev = qdev_create(NULL, "a9mpcore_priv");
    qdev_prop_set_uint32(dev, "num-cpu", smp_cpus);
    qdev_init_nofail(dev);
    busdev = sysbus_from_qdev(dev);
    vexpress_binfo.smp_priv_base = 0x1e000000;
    sysbus_mmio_map(busdev, 0, vexpress_binfo.smp_priv_base);
    for (n = 0; n < smp_cpus; n++) {
        sysbus_connect_irq(busdev, n, cpu_irq[n]);
    }
    /* Interrupts [42:0] are from the motherboard;
     * [47:43] are reserved; [63:48] are daughterboard
     * peripherals. Note that some documentation numbers
     * external interrupts starting from 32 (because the
     * A9MP has internal interrupts 0..31).
     */
    for (n = 0; n < 64; n++) {
        pic[n] = qdev_get_gpio_in(dev, n);
    }

    /* Motherboard peripherals: the wiring is the same but the
     * addresses vary between the legacy and A-Series memory maps.
     */

    sys_id = 0x1190f500;
    proc_id = 0x0c000191;

    sysctl = qdev_create(NULL, "realview_sysctl");
    qdev_prop_set_uint32(sysctl, "sys_id", sys_id);
    qdev_prop_set_uint32(sysctl, "proc_id", proc_id);
    qdev_init_nofail(sysctl);
    sysbus_mmio_map(sysbus_from_qdev(sysctl), 0, map[VE_SYSREGS]);

    /* VE_SP810: not modelled */
    /* VE_SERIALPCI: not modelled */

    pl041 = qdev_create(NULL, "pl041");
    qdev_prop_set_uint32(pl041, "nc_fifo_depth", 512);
    qdev_init_nofail(pl041);
    sysbus_mmio_map(sysbus_from_qdev(pl041), 0, map[VE_PL041]);
    sysbus_connect_irq(sysbus_from_qdev(pl041), 0, pic[11]);

    dev = sysbus_create_varargs("pl181", map[VE_MMCI], pic[9], pic[10], NULL);
    /* Wire up MMC card detect and read-only signals */
    qdev_connect_gpio_out(dev, 0,
                          qdev_get_gpio_in(sysctl, ARM_SYSCTL_GPIO_MMC_WPROT));
    qdev_connect_gpio_out(dev, 1,
                          qdev_get_gpio_in(sysctl, ARM_SYSCTL_GPIO_MMC_CARDIN));

    sysbus_create_simple("pl050_keyboard", map[VE_KMI0], pic[12]);
    sysbus_create_simple("pl050_mouse", map[VE_KMI1], pic[13]);

    sysbus_create_simple("pl011", map[VE_UART0], pic[5]);
    sysbus_create_simple("pl011", map[VE_UART1], pic[6]);
    sysbus_create_simple("pl011", map[VE_UART2], pic[7]);
    sysbus_create_simple("pl011", map[VE_UART3], pic[8]);

    sysbus_create_simple("sp804", map[VE_TIMER01], pic[2]);
    sysbus_create_simple("sp804", map[VE_TIMER23], pic[3]);

    /* VE_SERIALDVI: not modelled */

    sysbus_create_simple("pl031", map[VE_RTC], pic[4]); /* RTC */

    /* VE_COMPACTFLASH: not modelled */

    /* VE_CLCD: not modelled (we use the daughterboard CLCD only) */

    /* Daughterboard peripherals : 0x10020000 .. 0x20000000 */

    /* 0x10020000 PL111 CLCD (daughterboard) */
    sysbus_create_simple("pl111", 0x10020000, pic[44]);

    /* 0x10060000 AXI RAM */
    /* 0x100e0000 PL341 Dynamic Memory Controller */
    /* 0x100e1000 PL354 Static Memory Controller */
    /* 0x100e2000 System Configuration Controller */

    sysbus_create_simple("sp804", 0x100e4000, pic[48]);
    /* 0x100e5000 SP805 Watchdog module */
    /* 0x100e6000 BP147 TrustZone Protection Controller */
    /* 0x100e9000 PL301 'Fast' AXI matrix */
    /* 0x100ea000 PL301 'Slow' AXI matrix */
    /* 0x100ec000 TrustZone Address Space Controller */
    /* 0x10200000 CoreSight debug APB */
    /* 0x1e00a000 PL310 L2 Cache Controller */
    sysbus_create_varargs("l2x0", 0x1e00a000, NULL);

    /* VE_NORFLASH0: not modelled */
    /* VE_NORFLASH0ALIAS: not modelled */
    /* VE_NORFLASH1: not modelled */

    sram_size = 0x2000000;
    memory_region_init_ram(sram, "vexpress.sram", sram_size);
    vmstate_register_ram_global(sram);
    memory_region_add_subregion(sysmem, map[VE_SRAM], sram);

    vram_size = 0x800000;
    memory_region_init_ram(vram, "vexpress.vram", vram_size);
    vmstate_register_ram_global(vram);
    memory_region_add_subregion(sysmem, map[VE_VIDEORAM], vram);

    /* 0x4e000000 LAN9118 Ethernet */
    if (nd_table[0].vlan) {
        lan9118_init(&nd_table[0], map[VE_ETHERNET], pic[15]);
    }

    /* VE_USB: not modelled */

    /* VE_DAPROM: not modelled */

    vexpress_binfo.ram_size = ram_size;
    vexpress_binfo.kernel_filename = kernel_filename;
    vexpress_binfo.kernel_cmdline = kernel_cmdline;
    vexpress_binfo.initrd_filename = initrd_filename;
    vexpress_binfo.nb_cpus = smp_cpus;
    vexpress_binfo.board_id = VEXPRESS_BOARD_ID;
    vexpress_binfo.loader_start = 0x60000000;
    vexpress_binfo.smp_loader_start = map[VE_SRAM];
    vexpress_binfo.smp_bootreg_addr = map[VE_SYSREGS] + 0x30;
    arm_load_kernel(first_cpu, &vexpress_binfo);
}


static QEMUMachine vexpress_a9_machine = {
    .name = "vexpress-a9",
    .desc = "ARM Versatile Express for Cortex-A9",
    .init = vexpress_a9_init,
    .use_scsi = 1,
    .max_cpus = 4,
};

static void vexpress_machine_init(void)
{
    qemu_register_machine(&vexpress_a9_machine);
}

machine_init(vexpress_machine_init);

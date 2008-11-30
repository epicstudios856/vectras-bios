// Initialize PCI devices (on emulators)
//
// Copyright (C) 2008  Kevin O'Connor <kevin@koconnor.net>
// Copyright (C) 2006 Fabrice Bellard
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "util.h" // dprintf
#include "pci.h" // pci_config_readl
#include "biosvar.h" // GET_EBDA
#include "pci_ids.h" // PCI_VENDOR_ID_INTEL
#include "pci_regs.h" // PCI_COMMAND

#define PCI_ADDRESS_SPACE_MEM		0x00
#define PCI_ADDRESS_SPACE_IO		0x01
#define PCI_ADDRESS_SPACE_MEM_PREFETCH	0x08

#define PCI_ROM_SLOT 6
#define PCI_NUM_REGIONS 7

static u32 pci_bios_io_addr;
static u32 pci_bios_mem_addr;
static u32 pci_bios_bigmem_addr;
/* host irqs corresponding to PCI irqs A-D */
static u8 pci_irqs[4] = { 11, 9, 11, 9 };

static void pci_set_io_region_addr(u16 bdf, int region_num, u32 addr)
{
    u16 cmd;
    u32 ofs, old_addr;

    if ( region_num == PCI_ROM_SLOT ) {
        ofs = 0x30;
    }else{
        ofs = 0x10 + region_num * 4;
    }

    old_addr = pci_config_readl(bdf, ofs);

    pci_config_writel(bdf, ofs, addr);
    dprintf(1, "region %d: 0x%08x\n", region_num, addr);

    /* enable memory mappings */
    cmd = pci_config_readw(bdf, PCI_COMMAND);
    if ( region_num == PCI_ROM_SLOT )
        cmd |= 2;
    else if (old_addr & PCI_ADDRESS_SPACE_IO)
        cmd |= 1;
    else
        cmd |= 2;
    pci_config_writew(bdf, PCI_COMMAND, cmd);
}

/* return the global irq number corresponding to a given device irq
   pin. We could also use the bus number to have a more precise
   mapping. */
static int pci_slot_get_pirq(u16 bdf, int irq_num)
{
    int slot_addend = pci_bdf_to_dev(bdf) - 1;
    return (irq_num + slot_addend) & 3;
}

static void pci_bios_init_bridges(u16 bdf)
{
    u16 vendor_id = pci_config_readw(bdf, PCI_VENDOR_ID);
    u16 device_id = pci_config_readw(bdf, PCI_DEVICE_ID);

    if (vendor_id == PCI_VENDOR_ID_INTEL
        && (device_id == PCI_DEVICE_ID_INTEL_82371SB_0
            || device_id == PCI_DEVICE_ID_INTEL_82371AB_0)) {
        int i, irq;
        u8 elcr[2];

        /* PIIX3/PIIX4 PCI to ISA bridge */

        elcr[0] = 0x00;
        elcr[1] = 0x00;
        for(i = 0; i < 4; i++) {
            irq = pci_irqs[i];
            /* set to trigger level */
            elcr[irq >> 3] |= (1 << (irq & 7));
            /* activate irq remapping in PIIX */
            pci_config_writeb(bdf, 0x60 + i, irq);
        }
        outb(elcr[0], 0x4d0);
        outb(elcr[1], 0x4d1);
        dprintf(1, "PIIX3/PIIX4 init: elcr=%02x %02x\n",
                elcr[0], elcr[1]);
    }
}

static void pci_bios_init_device(u16 bdf)
{
    int class;
    u32 *paddr;
    int i, pin, pic_irq, vendor_id, device_id;

    class = pci_config_readw(bdf, PCI_CLASS_DEVICE);
    vendor_id = pci_config_readw(bdf, PCI_VENDOR_ID);
    device_id = pci_config_readw(bdf, PCI_DEVICE_ID);
    dprintf(1, "PCI: bus=%d devfn=0x%02x: vendor_id=0x%04x device_id=0x%04x\n"
            , pci_bdf_to_bus(bdf), pci_bdf_to_devfn(bdf), vendor_id, device_id);
    switch(class) {
    case 0x0101:
        if (vendor_id == PCI_VENDOR_ID_INTEL
            && (device_id == PCI_DEVICE_ID_INTEL_82371SB_1
                || device_id == PCI_DEVICE_ID_INTEL_82371AB)) {
            /* PIIX3/PIIX4 IDE */
            pci_config_writew(bdf, 0x40, 0x8000); // enable IDE0
            pci_config_writew(bdf, 0x42, 0x8000); // enable IDE1
            goto default_map;
        } else {
            /* IDE: we map it as in ISA mode */
            pci_set_io_region_addr(bdf, 0, 0x1f0);
            pci_set_io_region_addr(bdf, 1, 0x3f4);
            pci_set_io_region_addr(bdf, 2, 0x170);
            pci_set_io_region_addr(bdf, 3, 0x374);
        }
        break;
    case 0x0300:
        if (vendor_id != 0x1234)
            goto default_map;
        /* VGA: map frame buffer to default Bochs VBE address */
        pci_set_io_region_addr(bdf, 0, 0xE0000000);
        break;
    case 0x0800:
        /* PIC */
        if (vendor_id == PCI_VENDOR_ID_IBM) {
            /* IBM */
            if (device_id == 0x0046 || device_id == 0xFFFF) {
                /* MPIC & MPIC2 */
                pci_set_io_region_addr(bdf, 0, 0x80800000 + 0x00040000);
            }
        }
        break;
    case 0xff00:
        if (vendor_id == PCI_VENDOR_ID_APPLE &&
            (device_id == 0x0017 || device_id == 0x0022)) {
            /* macio bridge */
            pci_set_io_region_addr(bdf, 0, 0x80800000);
        }
        break;
    default:
    default_map:
        /* default memory mappings */
        for(i = 0; i < PCI_NUM_REGIONS; i++) {
            int ofs;
            u32 val, size;

            if (i == PCI_ROM_SLOT)
                ofs = 0x30;
            else
                ofs = 0x10 + i * 4;
            pci_config_writel(bdf, ofs, 0xffffffff);
            val = pci_config_readl(bdf, ofs);
            if (val != 0) {
                size = (~(val & ~0xf)) + 1;
                if (val & PCI_ADDRESS_SPACE_IO)
                    paddr = &pci_bios_io_addr;
                else if (size >= 0x04000000)
                    paddr = &pci_bios_bigmem_addr;
                else
                    paddr = &pci_bios_mem_addr;
                *paddr = ALIGN(*paddr, size);
                pci_set_io_region_addr(bdf, i, *paddr);
                *paddr += size;
            }
        }
        break;
    }

    /* map the interrupt */
    pin = pci_config_readb(bdf, PCI_INTERRUPT_PIN);
    if (pin != 0) {
        pin = pci_slot_get_pirq(bdf, pin - 1);
        pic_irq = pci_irqs[pin];
        pci_config_writeb(bdf, PCI_INTERRUPT_LINE, pic_irq);
    }

    if (vendor_id == PCI_VENDOR_ID_INTEL
        && device_id == PCI_DEVICE_ID_INTEL_82371AB_3) {
        /* PIIX4 Power Management device (for ACPI) */
        u32 pm_io_base = BUILD_PM_IO_BASE;
        pci_config_writel(bdf, 0x40, pm_io_base | 1);
        pci_config_writeb(bdf, 0x80, 0x01); /* enable PM io space */
        u32 smb_io_base = BUILD_SMB_IO_BASE;
        pci_config_writel(bdf, 0x90, smb_io_base | 1);
        pci_config_writeb(bdf, 0xd2, 0x09); /* enable SMBus io space */
    }
}

void
pci_bios_setup(void)
{
    if (CONFIG_COREBOOT)
        // Already done by coreboot.
        return;

    pci_bios_io_addr = 0xc000;
    pci_bios_mem_addr = 0xf0000000;
    pci_bios_bigmem_addr = GET_EBDA(ram_size);
    if (pci_bios_bigmem_addr < 0x90000000)
        pci_bios_bigmem_addr = 0x90000000;

    int bdf, max;
    foreachpci(bdf, max, 0) {
        pci_bios_init_bridges(bdf);
    }
    foreachpci(bdf, max, 0) {
        pci_bios_init_device(bdf);
    }
}

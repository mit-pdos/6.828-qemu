# Default configuration for x86_64-softmmu

include pci.mak
include usb.mak
CONFIG_VGA=y
CONFIG_QXL=$(CONFIG_SPICE)
CONFIG_VGA_PCI=y
CONFIG_VGA_ISA=y
CONFIG_VGA_CIRRUS=y
CONFIG_VMWARE_VGA=y
CONFIG_VMMOUSE=y
CONFIG_SERIAL=y
CONFIG_PARALLEL=y
CONFIG_I8254=y
CONFIG_PCSPK=y
CONFIG_PCKBD=y
CONFIG_FDC=y
CONFIG_ACPI=y
CONFIG_APM=y
CONFIG_I8257=y
CONFIG_IDE_ISA=y
CONFIG_IDE_PIIX=y
CONFIG_NE2000_ISA=y
CONFIG_PIIX_PCI=y
CONFIG_SOUND=y
CONFIG_HPET=y
CONFIG_APPLESMC=y
CONFIG_I8259=y
CONFIG_PFLASH_CFI01=y
CONFIG_TPM_TIS=y
CONFIG_TPM_PASSTHROUGH=y
CONFIG_PCI_HOTPLUG=y
CONFIG_MC146818RTC=y
CONFIG_WDT_IB700=y
CONFIG_PC_SYSFW=y
CONFIG_XEN_I386=$(CONFIG_XEN)
CONFIG_ISA_DEBUG=y
CONFIG_LPC_ICH9=y

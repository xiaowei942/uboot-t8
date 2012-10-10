/*
 * (C) Copyright 2010
 * Texas Instruments, <www.ti.com>
 *
 * Aneesh V <aneesh@ti.com>
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */
#include <common.h>
#include <asm/u-boot.h>
#include <asm/utils.h>
#include <asm/arch/sys_proto.h>
#include <mmc.h>
#include <fat.h>
#include <timestamp_autogenerated.h>
#include <version_autogenerated.h>
#include <asm/omap_common.h>
#include <asm/arch/mmc_host_def.h>
#include <i2c.h>
#include <image.h>

DECLARE_GLOBAL_DATA_PTR;

/* Define global data structure pointer to it*/
static gd_t gdata __attribute__ ((section(".data")));
static bd_t bdata __attribute__ ((section(".data")));
static const char *image_name;
static u8 image_os;
static u32 image_load_addr;
static u32 image_entry_point;
static u32 image_size;

inline void hang(void)
{
	puts("### ERROR ### Please RESET the board ###\n");
	for (;;)
		;
}

void board_init_f(ulong dummy)
{
	/*
	 * We call relocate_code() with relocation target same as the
	 * CONFIG_SYS_SPL_TEXT_BASE. This will result in relocation getting
	 * skipped. Instead, only .bss initialization will happen. That's
	 * all we need
	 */
	debug(">>board_init_f()\n");
	relocate_code(CONFIG_SPL_STACK, &gdata, CONFIG_SPL_TEXT_BASE);
}

#ifdef CONFIG_GENERIC_MMC
int board_mmc_init(bd_t *bis)
{
	switch (omap_boot_device()) {
	case BOOT_DEVICE_MMC1:
		omap_mmc_init(0);
		break;
	case BOOT_DEVICE_MMC2:
		omap_mmc_init(1);
		break;
	}
	return 0;
}
#endif

static void parse_image_header(const struct image_header *header)
{
	u32 header_size = sizeof(struct image_header);

	if (__be32_to_cpu(header->ih_magic) == IH_MAGIC) {
		image_size = __be32_to_cpu(header->ih_size) + header_size;
		image_entry_point = __be32_to_cpu(header->ih_load);
		/* Load including the header */
		image_load_addr = image_entry_point - header_size;
		image_os = header->ih_os;
		image_name = (const char *)&header->ih_name;
		debug("spl: payload image: %s load addr: 0x%x size: %d\n",
			image_name, image_load_addr, image_size);
	} else {
		/* Signature not found - assume u-boot.bin */
		printf("mkimage signature not found - ih_magic = %x\n",
			header->ih_magic);
		puts("Assuming u-boot.bin ..\n");
		/* Let's assume U-Boot will not be more than 200 KB */
		image_size = 200 * 1024;
		image_entry_point = CONFIG_SYS_TEXT_BASE;
		image_load_addr = CONFIG_SYS_TEXT_BASE;
		image_os = IH_OS_U_BOOT;
		image_name = "U-Boot";
	}
}

static void mmc_load_image_raw(struct mmc *mmc)
{
	u32 image_size_sectors, err;
	const struct image_header *header;

	header = (struct image_header *)(CONFIG_SYS_TEXT_BASE -
						sizeof(struct image_header));

	/* read image header to find the image size & load address */
	err = mmc->block_dev.block_read(0,
			CONFIG_SYS_MMCSD_RAW_MODE_U_BOOT_SECTOR, 1,
			(void *)header);

	if (err <= 0)
		goto end;

	parse_image_header(header);

	/* convert size to sectors - round up */
	image_size_sectors = (image_size + MMCSD_SECTOR_SIZE - 1) /
				MMCSD_SECTOR_SIZE;

	/* Read the header too to avoid extra memcpy */
	err = mmc->block_dev.block_read(0,
			CONFIG_SYS_MMCSD_RAW_MODE_U_BOOT_SECTOR,
			image_size_sectors, (void *)image_load_addr);

end:
	if (err <= 0) {
		printf("spl: mmc blk read err - %d\n", err);
		hang();
	}
}

static void mmc_load_image_fat(struct mmc *mmc)
{
	s32 err;
	struct image_header *header;

	header = (struct image_header *)(CONFIG_SYS_TEXT_BASE -
						sizeof(struct image_header));

	err = fat_register_device(&mmc->block_dev,
				CONFIG_SYS_MMC_SD_FAT_BOOT_PARTITION);
	if (err) {
		printf("spl: fat register err - %d\n", err);
		hang();
	}

	err = file_fat_read(CONFIG_SPL_FAT_LOAD_PAYLOAD_NAME,
				(u8 *)header, sizeof(struct image_header));
	if (err <= 0)
		goto end;

	parse_image_header(header);

	err = file_fat_read(CONFIG_SPL_FAT_LOAD_PAYLOAD_NAME,
				(u8 *)image_load_addr, 0);

end:
	if (err <= 0) {
		printf("spl: error reading image %s, err - %d\n",
			CONFIG_SPL_FAT_LOAD_PAYLOAD_NAME, err);
		hang();
	}
}

static void mmc_load_image(void)
{
	struct mmc *mmc;
	int err;
	u32 boot_mode;

	mmc_initialize(gd->bd);
	/* We register only one device. So, the dev id is always 0 */
	mmc = find_mmc_device(0);
	if (!mmc) {
		puts("spl: mmc device not found!!\n");
		hang();
	}

	err = mmc_init(mmc);
	if (err) {
		printf("spl: mmc init failed: err - %d\n", err);
		hang();
	}

	boot_mode = omap_boot_mode();
	if (boot_mode == MMCSD_MODE_RAW) {
		debug("boot mode - RAW\n");
		mmc_load_image_raw(mmc);
	} else if (boot_mode == MMCSD_MODE_FAT) {
		debug("boot mode - FAT\n");
		mmc_load_image_fat(mmc);
	} else {
		puts("spl: wrong MMC boot mode\n");
		hang();
	}
}

void jump_to_image_no_args(void)
{
	typedef void (*image_entry_noargs_t)(void)__attribute__ ((noreturn));
	image_entry_noargs_t image_entry =
			(image_entry_noargs_t) image_entry_point;

	image_entry();
}

void jump_to_image_no_args(void) __attribute__ ((noreturn));
void board_init_r(gd_t *id, ulong dummy)
{
	u32 boot_device;
	debug(">>spl:board_init_r()\n");

	timer_init();
	i2c_init(CONFIG_SYS_I2C_SPEED, CONFIG_SYS_I2C_SLAVE);

	boot_device = omap_boot_device();
	debug("boot device - %d\n", boot_device);
	switch (boot_device) {
	case BOOT_DEVICE_MMC1:
	case BOOT_DEVICE_MMC2:
		mmc_load_image();
		break;
	default:
		printf("SPL: Un-supported Boot Device - %d!!!\n", boot_device);
		hang();
		break;
	}

	switch (image_os) {
	case IH_OS_U_BOOT:
		debug("Jumping to U-Boot\n");
		jump_to_image_no_args();
		break;
	default:
		puts("Unsupported OS image.. Jumping nevertheless..\n");
		jump_to_image_no_args();
	}
}

void preloader_console_init(void)
{
	const char *u_boot_rev = U_BOOT_VERSION;
	char rev_string_buffer[50];

	gd = &gdata;
	gd->bd = &bdata;
	gd->flags |= GD_FLG_RELOC;
	gd->baudrate = CONFIG_BAUDRATE;

	setup_clocks_for_console();
	serial_init();		/* serial communications setup */

	/* Avoid a second "U-Boot" coming from this string */
	u_boot_rev = &u_boot_rev[7];

	printf("\nU-Boot SPL %s (%s - %s)\n", u_boot_rev, U_BOOT_DATE,
		U_BOOT_TIME);
	omap_rev_string(rev_string_buffer);
	printf("Texas Instruments %s\n", rev_string_buffer);
}

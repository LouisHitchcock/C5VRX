/* C5VRX by Twotoz and contributors.
 * The menu/idle raster is reserved at the RF dump bank 0x40830000 (video.c,
 * SOC_RESERVE_MEMORY_REGION) so the SNR meter can lend that bank to the dump
 * writer. Fail the link if static RAM would run into it. */
ASSERT(_bss_end <= 0x40830000,
       "C5VRX-4: static DRAM overlaps the menu raster / RF dump bank at 0x40830000")

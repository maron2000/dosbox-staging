// SPDX-FileCopyrightText:  2020-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "dosbox.h"
#include "inout.h"
#include "vga.h"

#define gfx(blah) vga.gfx.blah
static bool index9warned=false;

static void write_p3ce(io_port_t, io_val_t value, io_width_t)
{
	const auto val = check_cast<uint8_t>(value);
	gfx(index) = val & 0x0f;
}

static uint8_t read_p3ce(io_port_t, io_width_t)
{
	return gfx(index);
}

static void write_p3cf(io_port_t, io_val_t value, io_width_t)
{
	const auto val = check_cast<uint8_t>(value);
	switch (gfx(index)) {
	case 0:	/* Set/Reset Register */
		gfx(set_reset)=val & 0x0f;
		vga.config.full_set_reset=FillTable[val & 0x0f];
		vga.config.full_enable_and_set_reset=vga.config.full_set_reset &
			vga.config.full_enable_set_reset;
		/*
			0	If in Write Mode 0 and bit 0 of 3CEh index 1 is set a write to
				display memory will set all the bits in plane 0 of the byte to this
				bit, if the corresponding bit is set in the Map Mask Register (3CEh
				index 8).
			1	Same for plane 1 and bit 1 of 3CEh index 1.
			2	Same for plane 2 and bit 2 of 3CEh index 1.
			3	Same for plane 3 and bit 3 of 3CEh index 1.
		*/
//		LOG_DEBUG("Set Reset = %2X",val);
		break;
	case 1: /* Enable Set/Reset Register */
		gfx(enable_set_reset)=val & 0x0f;
		vga.config.full_enable_set_reset=FillTable[val & 0x0f];
		vga.config.full_not_enable_set_reset=~vga.config.full_enable_set_reset;
		vga.config.full_enable_and_set_reset=vga.config.full_set_reset &
			vga.config.full_enable_set_reset;
//		if (gfx(enable_set_reset)) vga.config.mh_mask|=MH_SETRESET else vga.config.mh_mask&=~MH_SETRESET;
		break;
	case 2: /* Color Compare Register */
		gfx(color_compare)=val & 0x0f;
		/*
			0-3	In Read Mode 1 each pixel at the address of the byte read is compared
				to this color and the corresponding bit in the output set to 1 if
				they match, 0 if not. The Color Don't Care Register (3CEh index 7)
				can exclude bitplanes from the comparison.
		*/
		vga.config.color_compare=val & 0xf;
//		LOG_DEBUG("Color Compare = %2X",val);
		break;
	case 3: /* Data Rotate */
		gfx(data_rotate)=val;
		vga.config.data_rotate=val & 7;
//		if (val) vga.config.mh_mask|=MH_ROTATEOP else vga.config.mh_mask&=~MH_ROTATEOP;
		vga.config.raster_op=(val>>3) & 3;
		/* 
			0-2	Number of positions to rotate data right before it is written to
				display memory. Only active in Write Mode 0.
			3-4	In Write Mode 2 this field controls the relation between the data
				written from the CPU, the data latched from the previous read and the
				data written to display memory:
				0: CPU Data is written unmodified
				1: CPU data is ANDed with the latched data
				2: CPU data is ORed  with the latch data.
				3: CPU data is XORed with the latched data.
		*/
		break;
	case 4: /* Read Map Select Register */
		/*	0-1	number of the plane Read Mode 0 will read from */
		gfx(read_map_select)=val & 0x03;
		vga.config.read_map_select=val & 0x03;
//		LOG_DEBUG("Read Map %2X",val);
		break;
	case 5: /* Mode Register */
		if ((gfx(mode) ^ val) & 0xf0) {
		gfx(mode)=val;
			VGA_DetermineMode();
		} else gfx(mode)=val;
		vga.config.write_mode=val & 3;
		vga.config.read_mode=(val >> 3) & 1;
//		LOG_DEBUG("Write Mode %d Read Mode %d val %d",vga.config.write_mode,vga.config.read_mode,val);
		/*
			0-1	Write Mode: Controls how data from the CPU is transformed before
				being written to display memory:
				0:	Mode 0 works as a Read-Modify-Write operation.
					First a read access loads the data latches of the VGA with the
					value in video memory at the addressed location. Then a write
					access will provide the destination address and the CPU data
					byte. The data written is modified by the function code in the
					Data Rotate register (3CEh index 3) as a function of the CPU
					data and the latches, then data is rotated as specified by the
					same register.
				1:	Mode 1 is used for video to video transfers.
					A read access will load the data latches with the contents of
					the addressed byte of video memory. A write access will write
					the contents of the latches to the addressed byte. Thus a single
					MOVSB instruction can copy all pixels in the source address byte
					to the destination address.
				2:	Mode 2 writes a color to all pixels in the addressed byte of
					video memory. Bit 0 of the CPU data is written to plane 0 et
					cetera. Individual bits can be enabled or disabled through the
					Bit Mask register (3CEh index 8).
				3:	Mode 3 can be used to fill an area with a color and pattern. The
					CPU data is rotated according to 3CEh index 3 bits 0-2 and anded
					with the Bit Mask Register (3CEh index 8). For each bit in the
					result the corresponding pixel is set to the color in the
					Set/Reset Register (3CEh index 0 bits 0-3) if the bit is set and
					to the contents of the processor latch if the bit is clear.
			3	Read Mode
				0:	Data is read from one of 4 bit planes depending on the Read Map
					Select Register (3CEh index 4).
				1:	Data returned is a comparison between the 8 pixels occupying the
					read byte and the color in the Color Compare Register (3CEh
					index 2). A bit is set if the color of the corresponding pixel
					matches the register.
			4	Enables Odd/Even mode if set (See 3C4h index 4 bit 2).
			5	Enables CGA style 4 color pixels using even/odd bit pairs if set.
			6	Enables 256 color mode if set.	
		*/
		break;
	case 6: /* Miscellaneous Register */
		if ((gfx(miscellaneous) ^ val) & 0x0c) {
			gfx(miscellaneous)=val;
			VGA_DetermineMode();
		} else gfx(miscellaneous)=val;
		VGA_SetupHandlers();
		/*
			0	Indicates Graphics Mode if set, Alphanumeric mode else.
			1	Enables Odd/Even mode if set.
			2-3	Memory Mapping:
				0: use A000h-BFFFh
				1: use A000h-AFFFh   VGA Graphics modes
				2: use B000h-B7FFh   Monochrome modes
				3: use B800h-BFFFh   CGA modes
		*/
		break;
	case 7: /* Color Don't Care Register */
		gfx(color_dont_care)=val & 0x0f;
		/*
			0	Ignore bit plane 0 in Read mode 1 if clear.
			1	Ignore bit plane 1 in Read mode 1 if clear.
			2	Ignore bit plane 2 in Read mode 1 if clear.
			3	Ignore bit plane 3 in Read mode 1 if clear.
		*/
		vga.config.color_dont_care=val & 0xf;
//		LOG_DEBUG("Color don't care = %2X",val);
		break;
	case 8: /* Bit Mask Register */
		gfx(bit_mask)=val;
		vga.config.full_bit_mask=ExpandTable[val];
//		LOG_DEBUG("Bit mask %2X",val);
		/*
			0-7	Each bit if set enables writing to the corresponding bit of a byte in
				display memory.
		*/
		break;
	default:
		if (svga.write_p3cf) {
			svga.write_p3cf(gfx(index), val, io_width_t::byte);
			break;
		}
		if (gfx(index) == 9 && !index9warned) {
			LOG(LOG_VGAMISC,LOG_NORMAL)("VGA:3CF:Write %2X to illegal index 9",val);
			index9warned=true;
			break;
		}
		LOG(LOG_VGAMISC,LOG_NORMAL)("VGA:3CF:Write %2X to illegal index %2X",val,gfx(index));
		break;
	}
}

static uint8_t read_p3cf(io_port_t port, io_width_t)
{
	switch (gfx(index)) {
	case 0:	/* Set/Reset Register */
		return gfx(set_reset);
	case 1: /* Enable Set/Reset Register */
		return gfx(enable_set_reset);
	case 2: /* Color Compare Register */
		return gfx(color_compare);
	case 3: /* Data Rotate */
		return gfx(data_rotate);
	case 4: /* Read Map Select Register */
		return gfx(read_map_select);
	case 5: /* Mode Register */
		return gfx(mode);
	case 6: /* Miscellaneous Register */
		return gfx(miscellaneous);
	case 7: /* Color Don't Care Register */
		return gfx(color_dont_care);
	case 8: /* Bit Mask Register */
		return gfx(bit_mask);
	default:
		if (svga.read_p3cf)
			return svga.read_p3cf(gfx(index), io_width_t::byte);
		LOG(LOG_VGAMISC,LOG_NORMAL)("Reading from illegal index %2X in port %4X",static_cast<uint32_t>(gfx(index)),port);
		break;
	}
	return 0;	/* Compiler happy */
}

void VGA_SetupGFX()
{
	if (is_machine_ega_or_better()) {
		IO_RegisterWriteHandler(0x3ce, write_p3ce, io_width_t::byte);
		IO_RegisterWriteHandler(0x3cf, write_p3cf, io_width_t::byte);
	}

	if (is_machine_vga_or_better()) {
		IO_RegisterReadHandler(0x3ce, read_p3ce, io_width_t::byte);
		IO_RegisterReadHandler(0x3cf, read_p3cf, io_width_t::byte);
	}
}

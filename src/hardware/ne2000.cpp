// SPDX-FileCopyrightText:  2021-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2008-2010  Ralf Grillenberger <h-a-l-9000@users.sourceforge.net>
// SPDX-FileCopyrightText:  2004-2008  Dean Beeler <canadacow@users.sourceforge.net>
// SPDX-FileCopyrightText:  2001-2004  Peter Grehan <grehan@iprg.nokia.com>
// SPDX-FileCopyrightText:  2001-2004  MandrakeSoft S.A.
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ne2000.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "callback.h"
#include "cpu.h"
#include "ethernet.h"
#include "inout.h"
#include "pic.h"
#include "setup.h"
#include "string_utils.h"
#include "support.h"
#include "timer.h"

/* Couldn't find a real spec for the NE2000 out there, hence this is adapted heavily from Bochs */

EthernetConnection* ethernet = nullptr;
static void NE2000_TX_Event(uint32_t val);

//Never completely fill the ne2k ring so that we never
// hit the unclear completely full buffer condition.
#define BX_NE2K_NEVER_FULL_RING (1)

#define LOG_THIS theNE2kDevice->
// #define BX_DEBUG
// #define BX_INFO
#define BX_NULL_TIMER_HANDLE 0
#define BX_ERROR LOG_WARNING
#define BX_PANIC LOG_WARNING
#define BX_RESET_HARDWARE 0
#define BX_RESET_SOFTWARE 1

static char bxtmp[1024];


static inline void BX_INFO(const char *msg, ...)
{
	va_list va;

	va_start(va, msg);
	safe_sprintf(bxtmp, msg, va);
	va_end(va);

	LOG(LOG_MISC, LOG_NORMAL)("NE2000: Info: %s", bxtmp);
}

static inline void BX_DEBUG(const char *msg, ...)
{
	if (false /*TOO MUCH DEBUG INFO*/) {
		va_list va;

		va_start(va, msg);
		safe_sprintf(bxtmp, msg, va);
		va_end(va);

		// LOG(LOG_MISC,LOG_DEBUG)("BX_DEBUG: %s",bxtmp);
	}
}

bx_ne2k_c* theNE2kDevice = nullptr;


bx_ne2k_c::bx_ne2k_c(void)
	: s()
{
  s.tx_timer_index = BX_NULL_TIMER_HANDLE;
}


bx_ne2k_c::~bx_ne2k_c(void)
{
  // nothing for now
}

//
// reset - restore state to power-up, cancelling all i/o
//
void
bx_ne2k_c::reset(unsigned type)
{
    (void)type;//UNUSED
  BX_DEBUG ("reset");
  // Assign register and memory states with initial values
  s.CR = bx_ne2k_t::CR_t{};
  s.ISR = bx_ne2k_t::ISR_t{};
  s.IMR = bx_ne2k_t::IMR_t{};
  s.DCR = bx_ne2k_t::DCR_t{};
  s.TCR = bx_ne2k_t::TCR_t{};
  s.TSR = bx_ne2k_t::TSR_t{};
  // s.RCR = bx_ne2k_t::RCR_t{};
  s.RSR = bx_ne2k_t::RSR_t{};

  BX_NE2K_THIS s.tx_timer_active = 0;
  BX_NE2K_THIS s.local_dma  = 0;
  BX_NE2K_THIS s.page_start = 0;
  BX_NE2K_THIS s.page_stop  = 0;
  BX_NE2K_THIS s.bound_ptr  = 0;
  BX_NE2K_THIS s.tx_page_start = 0;
  BX_NE2K_THIS s.num_coll   = 0;
  BX_NE2K_THIS s.tx_bytes   = 0;
  BX_NE2K_THIS s.fifo       = 0;
  BX_NE2K_THIS s.remote_dma = 0;
  BX_NE2K_THIS s.remote_start = 0;
  BX_NE2K_THIS s.remote_bytes = 0;
  BX_NE2K_THIS s.tallycnt_0 = 0;
  BX_NE2K_THIS s.tallycnt_1 = 0;
  BX_NE2K_THIS s.tallycnt_2 = 0;

  //memset( & BX_NE2K_THIS s.physaddr, 0, sizeof(BX_NE2K_THIS s.physaddr));
  //memset( & BX_NE2K_THIS s.mchash, 0, sizeof(BX_NE2K_THIS s.mchash));
  BX_NE2K_THIS s.curr_page = 0;

  BX_NE2K_THIS s.rempkt_ptr   = 0;
  BX_NE2K_THIS s.localpkt_ptr = 0;
  BX_NE2K_THIS s.address_cnt  = 0;

  memset( & BX_NE2K_THIS s.mem, 0, sizeof(BX_NE2K_THIS s.mem));

  // Set power-up conditions
  BX_NE2K_THIS s.CR.stop      = 1;
  BX_NE2K_THIS s.CR.rdma_cmd = 4;
  BX_NE2K_THIS s.ISR.reset    = 1;
  BX_NE2K_THIS s.DCR.longaddr = 1;
  PIC_DeActivateIRQ(s.base_irq);
  //DEV_pic_lower_irq(BX_NE2K_THIS s.base_irq);
}

//
// read_cr/write_cr - utility routines for handling reads/writes to
// the Command Register
//
uint32_t
bx_ne2k_c::read_cr(void)
{
	const uint32_t val = (             ((BX_NE2K_THIS s.CR.pgsel & 0x03u) << 6u) |
	                                   ((BX_NE2K_THIS s.CR.rdma_cmd & 0x07u) << 3u) |
	                      (unsigned int)(BX_NE2K_THIS s.CR.tx_packet << 2u) |
	                      (unsigned int)(BX_NE2K_THIS s.CR.start << 1u) |
	                      (unsigned int)(BX_NE2K_THIS s.CR.stop));
	BX_DEBUG("read CR returns 0x%08x", val);
	return val;
}

void
bx_ne2k_c::write_cr(io_val_t data)
{
  auto value = check_cast<uint8_t>(data);

  BX_DEBUG ("wrote 0x%02x to CR", value);

  // Validate remote-DMA
  if ((value & 0x38) == 0x00) {
    BX_DEBUG("CR write - invalid rDMA value 0");
    value |= 0x20; /* dma_cmd == 4 is a safe default */
	//value = 0x22; /* dma_cmd == 4 is a safe default */
  }

  // Check for s/w reset
  if (value & 0x01) {
    BX_NE2K_THIS s.ISR.reset = 1;
    BX_NE2K_THIS s.CR.stop   = 1;
  } else {
    BX_NE2K_THIS s.CR.stop = 0;
  }

  BX_NE2K_THIS s.CR.rdma_cmd = (value & 0x38) >> 3;

  // If start command issued, the RST bit in the ISR
  // must be cleared
  if ((value & 0x02) && !BX_NE2K_THIS s.CR.start) {
    BX_NE2K_THIS s.ISR.reset = 0;
  }

  BX_NE2K_THIS s.CR.start = ((value & 0x02) == 0x02);
  BX_NE2K_THIS s.CR.pgsel = (value & 0xc0) >> 6;

    // Check for send-packet command
    if (BX_NE2K_THIS s.CR.rdma_cmd == 3) {
	// Set up DMA read from receive ring
		BX_NE2K_THIS s.remote_start = BX_NE2K_THIS s.remote_dma =
			BX_NE2K_THIS s.bound_ptr * 256;
		BX_NE2K_THIS s.remote_bytes = * reinterpret_cast<uint16_t*>( &
			BX_NE2K_THIS s.mem[BX_NE2K_THIS s.bound_ptr * 256 + 2 - BX_NE2K_MEMSTART]);
		BX_INFO("Sending buffer #x%x length %d",
			BX_NE2K_THIS s.remote_start,
			BX_NE2K_THIS s.remote_bytes);
    }

  // Check for start-tx
    if ((value & 0x04) && BX_NE2K_THIS s.TCR.loop_cntl) {
		// loopback mode
		if (BX_NE2K_THIS s.TCR.loop_cntl != 1) {
			BX_INFO("Loop mode %d not supported.", BX_NE2K_THIS s.TCR.loop_cntl);
		} else {
			rx_frame (& BX_NE2K_THIS s.mem[BX_NE2K_THIS s.tx_page_start*256 -
				BX_NE2K_MEMSTART],
				BX_NE2K_THIS s.tx_bytes);

			// do a TX interrupt
			// Generate an interrupt if not masked and not one in progress
			if (BX_NE2K_THIS s.IMR.tx_inte && !BX_NE2K_THIS s.ISR.pkt_tx) {
				//LOG_MSG("tx complete interrupt");
				PIC_ActivateIRQ(s.base_irq);
			}
			BX_NE2K_THIS s.ISR.pkt_tx = 1;
		}
    }
    else if (value & 0x04) {
      // start-tx and no loopback
      if (BX_NE2K_THIS s.CR.stop || !BX_NE2K_THIS s.CR.start)
        BX_PANIC(("CR write - tx start, dev in reset"));

      if (BX_NE2K_THIS s.tx_bytes == 0)
        BX_PANIC(("CR write - tx start, tx bytes == 0"));

#ifdef notdef
      // XXX debug stuff
      printf("packet tx (%d bytes):\t", BX_NE2K_THIS s.tx_bytes);
      for (int i = 0; i < BX_NE2K_THIS s.tx_bytes; i++) {
        printf("%02x ", BX_NE2K_THIS s.mem[BX_NE2K_THIS s.tx_page_start*256 -
          BX_NE2K_MEMSTART + i]);
        if (i && (((i+1) % 16) == 0))
          printf("\t");
      }
      printf("");
#endif

      // If a scheduled transmission is still queued, then
      // send it now to ensure order is maintained.
      if (BX_NE2K_THIS s.tx_timer_active) {
	      PIC_RemoveEvents(NE2000_TX_Event);
	      NE2000_TX_Event(0);
	      LOG_MSG("NE2000: Preemptive transmit to retain packet order");
      }

      // Send the packet to the system driver
      // BX_NE2K_THIS ethdev->sendpkt(& BX_NE2K_THIS s.mem[BX_NE2K_THIS
      // s.tx_page_start*256 - BX_NE2K_MEMSTART], BX_NE2K_THIS s.tx_bytes);
      ethernet->SendPacket(&s.mem[s.tx_page_start * 256 - BX_NE2K_MEMSTART], s.tx_bytes);
      // s.tx_timer_index = (64 + 96 + 4*8 + BX_NE2K_THIS s.tx_bytes*8)/10;
      s.tx_timer_active = 1;

      // Schedule a timer to trigger a tx-complete interrupt
      // The number of microseconds is the bit-time / 100.
      // The bit-time is the preamble+sfd (64 bits), the
      // inter-frame gap (96 bits), the CRC (4 bytes), and the
      // the number of bits in the frame (s.tx_bytes * 8).
      //
      const auto tx_bits = 64 + 96 + 4 * 8 + s.tx_bytes * 8;
      const auto tx_usec = tx_bits / (100 * 1000.0);
      PIC_AddEvent(NE2000_TX_Event, tx_usec, 0);
    }

  // Linux probes for an interrupt by setting up a remote-DMA read
  // of 0 bytes with remote-DMA completion interrupts enabled.
  // Detect this here
  if (BX_NE2K_THIS s.CR.rdma_cmd == 0x01 &&
      BX_NE2K_THIS s.CR.start &&
      BX_NE2K_THIS s.remote_bytes == 0) {
    BX_NE2K_THIS s.ISR.rdma_done = 1;
    if (BX_NE2K_THIS s.IMR.rdma_inte) {
  		PIC_ActivateIRQ(s.base_irq);
      //DEV_pic_raise_irq(BX_NE2K_THIS s.base_irq);
    }
  }
}

//
// chipmem_read/chipmem_write - access the 64K private RAM.
// The ne2000 memory is accessed through the data port of
// the asic (offset 0) after setting up a remote-DMA transfer.
// Both byte and word accesses are allowed.
// The first 16 bytes contains the MAC address at even locations,
// and there is 16K of buffer memory starting at 16K
//
io_val_t bx_ne2k_c::chipmem_read(io_port_t address, io_width_t io_len)
{
  uint32_t retval = 0;

  if ((io_len == io_width_t::word) && (address & 0x1))
    BX_PANIC(("unaligned chipmem word read"));

  // ROM'd MAC address
  if (/*(address >=0) && */address <= 31) {
    retval = BX_NE2K_THIS s.macaddr[address];
    if ((io_len == io_width_t::word) || (io_len == io_width_t::dword)) {
      retval |= (unsigned int)(BX_NE2K_THIS s.macaddr[address + 1u] << 8u);
	  if (io_len == io_width_t::dword) {
			retval |= (unsigned int)(BX_NE2K_THIS s.macaddr[address + 2u] << 16u);
			retval |= (unsigned int)(BX_NE2K_THIS s.macaddr[address + 3u] << 24u);
	  }
    }
    return (retval);
  }

  if ((address >= BX_NE2K_MEMSTART) && (address < BX_NE2K_MEMEND)) {
    retval = BX_NE2K_THIS s.mem[address - BX_NE2K_MEMSTART];
    if ((io_len == io_width_t::word) || (io_len == io_width_t::dword)) {
      retval |= (unsigned int)(BX_NE2K_THIS s.mem[address - BX_NE2K_MEMSTART + 1] << 8u);
    }
	if (io_len == io_width_t::dword) {
       retval |= (unsigned int)(BX_NE2K_THIS s.mem[address - BX_NE2K_MEMSTART + 2] << 16u);
       retval |= (unsigned int)(BX_NE2K_THIS s.mem[address - BX_NE2K_MEMSTART + 3] << 24u);
    }
    return (retval);
  }

  BX_DEBUG("out-of-bounds chipmem read, %04X", address);

  return (0xff);
}

void
bx_ne2k_c::chipmem_write(io_port_t address, io_val_t data, io_width_t io_len)
{
  const auto value = check_cast<uint16_t>(data);
  if ((io_len == io_width_t::word) && (address & 0x1))
    BX_PANIC(("unaligned chipmem word write"));

  if ((address >= BX_NE2K_MEMSTART) && (address < BX_NE2K_MEMEND)) {
    BX_NE2K_THIS s.mem[address - BX_NE2K_MEMSTART] = value & 0xff;
    if (io_len == io_width_t::word)
      BX_NE2K_THIS s.mem[address - BX_NE2K_MEMSTART + 1] = value >> 8;
  } else
    BX_DEBUG("out-of-bounds chipmem write, %04X", address);
}

//
// asic_read/asic_write - This is the high 16 bytes of i/o space
// (the lower 16 bytes is for the DS8390). Only two locations
// are used: offset 0, which is used for data transfer, and
// offset 0xf, which is used to reset the device.
// The data transfer port is used to as 'external' DMA to the
// DS8390. The chip has to have the DMA registers set up, and
// after that, insw/outsw instructions can be used to move
// the appropriate number of bytes to/from the device.
//
uint32_t
bx_ne2k_c::asic_read(io_port_t offset, io_width_t io_len)
{
  uint32_t retval = 0;

  switch (offset) {
  case 0x0:  // Data register
    //
    // A read remote-DMA command must have been issued,
    // and the source-address and length registers must
    // have been initialised.
    //
    if (!s.remote_bytes) {
	    LOG_WARNING("NE2000: Empty ASIC read from port=0x%02x of length %u and %u remote_bytes",
			offset, enum_val(io_len), s.remote_bytes);
	    break;
    }

    //BX_INFO(("ne2k read DMA: addr=%4x remote_bytes=%d",BX_NE2K_THIS s.remote_dma,BX_NE2K_THIS s.remote_bytes));
    if (s.remote_bytes == 1)
	    io_len = io_width_t::byte;
    retval = chipmem_read(BX_NE2K_THIS s.remote_dma, io_len);
    //
    // The 8390 bumps the address and decreases the byte count
    // by the selected word size after every access, not by
    // the amount of data requested by the host (io_len).
    //
    BX_NE2K_THIS s.remote_dma += (BX_NE2K_THIS s.DCR.wdsize + 1);
    if (BX_NE2K_THIS s.remote_dma == BX_NE2K_THIS s.page_stop << 8) {
      BX_NE2K_THIS s.remote_dma = check_cast<uint16_t>(BX_NE2K_THIS s.page_start << 8);
    }
    // keep s.remote_bytes from underflowing
    if (BX_NE2K_THIS s.remote_bytes > 1)
      BX_NE2K_THIS s.remote_bytes -= (BX_NE2K_THIS s.DCR.wdsize + 1);
    else
      BX_NE2K_THIS s.remote_bytes = 0;

	// If all bytes have been written, signal remote-DMA complete
	if (BX_NE2K_THIS s.remote_bytes == 0) {
	    BX_NE2K_THIS s.ISR.rdma_done = 1;
	    if (BX_NE2K_THIS s.IMR.rdma_inte) {
			PIC_ActivateIRQ(s.base_irq);
		//DEV_pic_raise_irq(BX_NE2K_THIS s.base_irq);
		}
	}
    break;

  case 0xf:  // Reset register
    theNE2kDevice->reset(BX_RESET_SOFTWARE);
	//retval=0x1;
    break;

  default:
    BX_INFO("asic read invalid address %04x", (unsigned) offset);
    break;
  }

  return (retval);
}

void
bx_ne2k_c::asic_write(io_port_t offset, io_val_t value, io_width_t io_len)
{
	BX_DEBUG("asic write addr=0x%02x, value=0x%04x", offset, value);
	switch (offset) {
	case 0x0: // Data register - see asic_read for a description

		if ((io_len == io_width_t::word) && (BX_NE2K_THIS s.DCR.wdsize == 0)) {
			BX_PANIC(("dma write length 2 on byte mode operation"));
			break;
		}

		if (BX_NE2K_THIS s.remote_bytes == 0)
			BX_PANIC(("ne2K: dma write, byte count 0"));

		chipmem_write(BX_NE2K_THIS s.remote_dma, value, io_len);
		// is this right ??? asic_read uses DCR.wordsize
		BX_NE2K_THIS s.remote_dma += enum_val(io_len);
		if (BX_NE2K_THIS s.remote_dma == BX_NE2K_THIS s.page_stop << 8) {
			BX_NE2K_THIS s.remote_dma = check_cast<uint16_t>(
			        BX_NE2K_THIS s.page_start << 8);
		}

		BX_NE2K_THIS s.remote_bytes -= enum_val(io_len);
		if (BX_NE2K_THIS s.remote_bytes > BX_NE2K_MEMSIZ)
			BX_NE2K_THIS s.remote_bytes = 0;

		// If all bytes have been written, signal remote-DMA complete
		if (BX_NE2K_THIS s.remote_bytes == 0) {
			BX_NE2K_THIS s.ISR.rdma_done = 1;
			if (BX_NE2K_THIS s.IMR.rdma_inte) {
				PIC_ActivateIRQ(s.base_irq);
				// DEV_pic_raise_irq(BX_NE2K_THIS s.base_irq);
			}
		}
		break;

	case 0xf: // Reset register
		theNE2kDevice->reset(BX_RESET_SOFTWARE);
		break;

	default: // this is invalid, but happens under win95 device detection
		BX_INFO("asic write invalid address %04x, ignoring", (unsigned)offset);
		break;
	}
}

//
// page0_read/page0_write - These routines handle reads/writes to
// the 'zeroth' page of the DS8390 register file
//
uint32_t
bx_ne2k_c::page0_read(io_port_t offset, io_width_t io_len)
{
	BX_DEBUG("NE2000: page 0 read from port %04x, len=%u", (unsigned)offset,
	         (unsigned)io_len);
	if (enum_val(io_len) > 1) {
		BX_ERROR("NE2000: bad length! page 0 read from port %04x, len=%u",
		         (unsigned)offset, (unsigned)io_len); /* encountered
		                                                 with win98
		                                                 hardware probe */
		return 0;
  }


  switch (offset) {
  case 0x1:  // CLDA0
    return (BX_NE2K_THIS s.local_dma & 0xff);
    break;

  case 0x2:  // CLDA1
    return (unsigned int)(BX_NE2K_THIS s.local_dma >> 8u);
    break;

  case 0x3:  // BNRY
    return (BX_NE2K_THIS s.bound_ptr);
    break;

  case 0x4:  // TSR
    return
       ((unsigned int)(BX_NE2K_THIS s.TSR.ow_coll    << 7u) |
	    (unsigned int)(BX_NE2K_THIS s.TSR.cd_hbeat   << 6u) |
	    (unsigned int)(BX_NE2K_THIS s.TSR.fifo_ur    << 5u) |
	    (unsigned int)(BX_NE2K_THIS s.TSR.no_carrier << 4u) |
	    (unsigned int)(BX_NE2K_THIS s.TSR.aborted    << 3u) |
	    (unsigned int)(BX_NE2K_THIS s.TSR.collided   << 2u) |
	    (unsigned int)(BX_NE2K_THIS s.TSR.tx_ok));
    break;

  case 0x5:  // NCR
    return (BX_NE2K_THIS s.num_coll);
    break;

  case 0x6:  // FIFO
    // reading FIFO is only valid in loopback mode
    BX_ERROR(("reading FIFO not supported yet"));
    return (BX_NE2K_THIS s.fifo);
    break;

  case 0x7:  // ISR
    return
       ((unsigned int)(BX_NE2K_THIS s.ISR.reset     << 7u) |
	    (unsigned int)(BX_NE2K_THIS s.ISR.rdma_done << 6u) |
	    (unsigned int)(BX_NE2K_THIS s.ISR.cnt_oflow << 5u) |
	    (unsigned int)(BX_NE2K_THIS s.ISR.overwrite << 4u) |
	    (unsigned int)(BX_NE2K_THIS s.ISR.tx_err    << 3u) |
	    (unsigned int)(BX_NE2K_THIS s.ISR.rx_err    << 2u) |
	    (unsigned int)(BX_NE2K_THIS s.ISR.pkt_tx    << 1u) |
	    (unsigned int)(BX_NE2K_THIS s.ISR.pkt_rx));
    break;

  case 0x8:  // CRDA0
    return (BX_NE2K_THIS s.remote_dma & 0xff);
    break;

  case 0x9:  // CRDA1
    return (unsigned int)(BX_NE2K_THIS s.remote_dma >> 8u);
    break;

  case 0xa:  // reserved
    BX_INFO(("reserved read - page 0, 0xa"));
    return (0xff);
    break;

  case 0xb:  // reserved
    BX_INFO(("reserved read - page 0, 0xb"));
    return (0xff);
    break;

  case 0xc:  // RSR
    return
       ((unsigned int)(BX_NE2K_THIS s.RSR.deferred    << 7u) |
	    (unsigned int)(BX_NE2K_THIS s.RSR.rx_disabled << 6u) |
	    (unsigned int)(BX_NE2K_THIS s.RSR.rx_mbit     << 5u) |
	    (unsigned int)(BX_NE2K_THIS s.RSR.rx_missed   << 4u) |
	    (unsigned int)(BX_NE2K_THIS s.RSR.fifo_or     << 3u) |
	    (unsigned int)(BX_NE2K_THIS s.RSR.bad_falign  << 2u) |
	    (unsigned int)(BX_NE2K_THIS s.RSR.bad_crc     << 1u) |
	    (unsigned int)(BX_NE2K_THIS s.RSR.rx_ok));
    break;

  case 0xd:  // CNTR0
    return (BX_NE2K_THIS s.tallycnt_0);
    break;

  case 0xe:  // CNTR1
    return (BX_NE2K_THIS s.tallycnt_1);
    break;

  case 0xf:  // CNTR2
    return (BX_NE2K_THIS s.tallycnt_2);
    break;

  default:
    BX_PANIC("page 0 offset %04x out of range", (unsigned) offset);
  }

  return(0);
}

void
bx_ne2k_c::page0_write(io_port_t offset, io_val_t data, io_width_t io_len)
{
  BX_DEBUG("page 0 write to port %04x, len=%u", (unsigned)offset, (unsigned)io_len);

  // It appears to be a common practice to use outw on page0 regs...

  // break up outw into two outb's
  if (io_len == io_width_t::word) {
    const auto value = check_cast<uint16_t>(data);
    page0_write(offset, (value & 0xff), io_width_t::byte);
    page0_write(offset + 1, ((value >> 8) & 0xff), io_width_t::byte);
    return;
  }

  auto value = check_cast<uint8_t>(data);
  unsigned int new_value = 0;

  switch (offset) {
  case 0x1:  // PSTART
    BX_NE2K_THIS s.page_start = value;
    break;

  case 0x2:  // PSTOP
	// BX_INFO(("Writing to PSTOP: %02x", value));
    BX_NE2K_THIS s.page_stop = value;
    break;

  case 0x3:  // BNRY
    BX_NE2K_THIS s.bound_ptr = value;
    break;

  case 0x4:  // TPSR
    BX_NE2K_THIS s.tx_page_start = value;
    break;

  case 0x5:  // TBCR0
    // Clear out low byte and re-insert
    BX_NE2K_THIS s.tx_bytes &= 0xff00;
    BX_NE2K_THIS s.tx_bytes |= (value & 0xff);
    break;

  case 0x6:  // TBCR1
    // Clear out high byte and re-insert
    BX_NE2K_THIS s.tx_bytes &= 0x00ff;
    BX_NE2K_THIS s.tx_bytes |= ((value & 0xff) << 8);
    break;

  case 0x7:  // ISR
    value &= 0x7f;  // clear RST bit - status-only bit
    // All other values are cleared iff the ISR bit is 1
    BX_NE2K_THIS s.ISR.pkt_rx    &= ~((bx_bool)((value & 0x01) == 0x01));
    BX_NE2K_THIS s.ISR.pkt_tx    &= ~((bx_bool)((value & 0x02) == 0x02));
    BX_NE2K_THIS s.ISR.rx_err    &= ~((bx_bool)((value & 0x04) == 0x04));
    BX_NE2K_THIS s.ISR.tx_err    &= ~((bx_bool)((value & 0x08) == 0x08));
    BX_NE2K_THIS s.ISR.overwrite &= ~((bx_bool)((value & 0x10) == 0x10));
    BX_NE2K_THIS s.ISR.cnt_oflow &= ~((bx_bool)((value & 0x20) == 0x20));
    BX_NE2K_THIS s.ISR.rdma_done &= ~((bx_bool)((value & 0x40) == 0x40));
    new_value = ((unsigned int)(BX_NE2K_THIS s.ISR.rdma_done << 6u) |
		 (unsigned int)(BX_NE2K_THIS s.ISR.cnt_oflow << 5u) |
		 (unsigned int)(BX_NE2K_THIS s.ISR.overwrite << 4u) |
		 (unsigned int)(BX_NE2K_THIS s.ISR.tx_err << 3u) |
		 (unsigned int)(BX_NE2K_THIS s.ISR.rx_err << 2u) |
		 (unsigned int)(BX_NE2K_THIS s.ISR.pkt_tx << 1u) |
		 (unsigned int)(BX_NE2K_THIS s.ISR.pkt_rx));
    new_value &= ((unsigned int)(BX_NE2K_THIS s.IMR.rdma_inte << 6u) |
		  (unsigned int)(BX_NE2K_THIS s.IMR.cofl_inte << 5u) |
		  (unsigned int)(BX_NE2K_THIS s.IMR.overw_inte << 4u) |
		  (unsigned int)(BX_NE2K_THIS s.IMR.txerr_inte << 3u) |
		  (unsigned int)(BX_NE2K_THIS s.IMR.rxerr_inte << 2u) |
		  (unsigned int)(BX_NE2K_THIS s.IMR.tx_inte << 1u) |
		  (unsigned int)(BX_NE2K_THIS s.IMR.rx_inte));
    if (new_value == 0)
	    PIC_DeActivateIRQ(s.base_irq);
    // DEV_pic_lower_irq(BX_NE2K_THIS s.base_irq);
    value = check_cast<uint8_t>(new_value);
    break;

  case 0x8:  // RSAR0
    // Clear out low byte and re-insert
    BX_NE2K_THIS s.remote_start &= 0xff00u;
    BX_NE2K_THIS s.remote_start |= (value & 0xffu);
    BX_NE2K_THIS s.remote_dma = BX_NE2K_THIS s.remote_start;
    break;

  case 0x9:  // RSAR1
    // Clear out high byte and re-insert
    BX_NE2K_THIS s.remote_start &= 0x00ffu;
    BX_NE2K_THIS s.remote_start |= ((value & 0xffu) << 8u);
    BX_NE2K_THIS s.remote_dma = BX_NE2K_THIS s.remote_start;
    break;

  case 0xa:  // RBCR0
    // Clear out low byte and re-insert
    BX_NE2K_THIS s.remote_bytes &= 0xff00u;
    BX_NE2K_THIS s.remote_bytes |= (value & 0xffu);
    break;

  case 0xb:  // RBCR1
    // Clear out high byte and re-insert
    BX_NE2K_THIS s.remote_bytes &= 0x00ffu;
    BX_NE2K_THIS s.remote_bytes |= ((value & 0xffu) << 8u);
    break;

  case 0xc:  // RCR
    // Check if the reserved bits are set
    if (value & 0xc0)
      BX_INFO(("RCR write, reserved bits set"));

    // Set all other bit-fields
    BX_NE2K_THIS s.RCR.errors_ok = ((value & 0x01u) == 0x01u);
    BX_NE2K_THIS s.RCR.runts_ok  = ((value & 0x02u) == 0x02u);
    BX_NE2K_THIS s.RCR.broadcast = ((value & 0x04u) == 0x04u);
    BX_NE2K_THIS s.RCR.multicast = ((value & 0x08u) == 0x08u);
    BX_NE2K_THIS s.RCR.promisc   = ((value & 0x10u) == 0x10u);
    BX_NE2K_THIS s.RCR.monitor   = ((value & 0x20u) == 0x20u);

    // Monitor bit is a little suspicious...
    if (value & 0x20)
	    BX_INFO(("NE2000: RCR write, monitor bit set!"));
    break;

  case 0xd:  // TCR
    // Check reserved bits
    if (value & 0xe0)
	    BX_ERROR(("NE2000: TCR write, reserved bits set"));

    // Test loop mode (not supported)
    if (value & 0x06) {
      BX_NE2K_THIS s.TCR.loop_cntl = (value & 0x6) >> 1;
      BX_INFO("TCR write, loop mode %d not supported", BX_NE2K_THIS s.TCR.loop_cntl);
    } else {
      BX_NE2K_THIS s.TCR.loop_cntl = 0;
    }

    // Inhibit-CRC not supported.
    if (value & 0x01)
      BX_PANIC(("TCR write, inhibit-CRC not supported"));

    // Auto-transmit disable very suspicious
    if (value & 0x08)
      BX_PANIC(("TCR write, auto transmit disable not supported"));

    // Allow collision-offset to be set, although not used
    BX_NE2K_THIS s.TCR.coll_prio = ((value & 0x08) == 0x08);
    break;

  case 0xe:  // DCR
    // the loopback mode is not suppported yet
    if (!(value & 0x08)) {
	    BX_ERROR(("NE2000: DCR write, loopback mode selected"));
    }
    // It is questionable to set longaddr and auto_rx, since they
    // aren't supported on the ne2000. Print a warning and continue
    if (value & 0x04)
      BX_INFO(("DCR write - LAS set ???"));
    if (value & 0x10)
      BX_INFO(("DCR write - AR set ???"));

    // Set other values.
    BX_NE2K_THIS s.DCR.wdsize   = ((value & 0x01) == 0x01);
    BX_NE2K_THIS s.DCR.endian   = ((value & 0x02) == 0x02);
    BX_NE2K_THIS s.DCR.longaddr = ((value & 0x04) == 0x04); // illegal ?
    BX_NE2K_THIS s.DCR.loop     = ((value & 0x08) == 0x08);
    BX_NE2K_THIS s.DCR.auto_rx  = ((value & 0x10) == 0x10); // also illegal ?
    BX_NE2K_THIS s.DCR.fifo_size = (value & 0x50) >> 5;
    break;

  case 0xf:  // IMR
    // Check for reserved bit
	if (value & 0x80)
      BX_PANIC(("IMR write, reserved bit set"));

    // Set other values
    BX_NE2K_THIS s.IMR.rx_inte    = ((value & 0x01) == 0x01);
    BX_NE2K_THIS s.IMR.tx_inte    = ((value & 0x02) == 0x02);
    BX_NE2K_THIS s.IMR.rxerr_inte = ((value & 0x04) == 0x04);
    BX_NE2K_THIS s.IMR.txerr_inte = ((value & 0x08) == 0x08);
    BX_NE2K_THIS s.IMR.overw_inte = ((value & 0x10) == 0x10);
    BX_NE2K_THIS s.IMR.cofl_inte  = ((value & 0x20) == 0x20);
    BX_NE2K_THIS s.IMR.rdma_inte  = ((value & 0x40) == 0x40);
    if (BX_NE2K_THIS s.ISR.pkt_tx && BX_NE2K_THIS s.IMR.tx_inte) {
	    PIC_ActivateIRQ(s.base_irq);
    }
    break;
  default:
    BX_PANIC("page 0 write, bad offset %0x", offset);
  }
}


//
// page1_read/page1_write - These routines handle reads/writes to
// the first page of the DS8390 register file
//
uint32_t
bx_ne2k_c::page1_read(io_port_t offset, io_width_t io_len)
{
  BX_DEBUG("page 1 read from port %04x, len=%u", (unsigned) offset,
	   (unsigned) io_len);
  if (enum_val(io_len) > 1)
    BX_PANIC("bad length! page 1 read from port %04x, len=%u", (unsigned) offset,
             (unsigned) io_len);

  switch (offset) {
  case 0x1:  // PAR0-5
  case 0x2:
  case 0x3:
  case 0x4:
  case 0x5:
  case 0x6:
    return (BX_NE2K_THIS s.physaddr[offset - 1]);
    break;

  case 0x7:  // CURR
      BX_DEBUG("returning current page: %02x", (BX_NE2K_THIS s.curr_page));
    return (BX_NE2K_THIS s.curr_page);

  case 0x8:  // MAR0-7
  case 0x9:
  case 0xa:
  case 0xb:
  case 0xc:
  case 0xd:
  case 0xe:
  case 0xf:
    return (BX_NE2K_THIS s.mchash[offset - 8]);
    break;

  default:
    BX_PANIC("page 1 r offset %04x out of range", (unsigned) offset);
  }

  return (0);
}

void
bx_ne2k_c::page1_write(io_port_t offset, io_val_t data, io_width_t io_len)
{
  const auto value = check_cast<uint8_t>(data);
    (void)io_len;//UNUSED
  BX_DEBUG("page 1 w offset %04x", (unsigned) offset);
  switch (offset) {
  case 0x1:  // PAR0-5
  case 0x2:
  case 0x3:
  case 0x4:
  case 0x5:
  case 0x6:
    BX_NE2K_THIS s.physaddr[offset - 1] = value;
    break;

  case 0x7:  // CURR
    BX_NE2K_THIS s.curr_page = value;
    break;

  case 0x8:  // MAR0-7
  case 0x9:
  case 0xa:
  case 0xb:
  case 0xc:
  case 0xd:
  case 0xe:
  case 0xf:
    BX_NE2K_THIS s.mchash[offset - 8] = value;
    break;

  default:
    BX_PANIC("page 1 w offset %04x out of range", (unsigned) offset);
  }
}


//
// page2_read/page2_write - These routines handle reads/writes to
// the second page of the DS8390 register file
//
uint32_t
bx_ne2k_c::page2_read(io_port_t offset, io_width_t io_len)
{
  BX_DEBUG("page 2 read from port %04x, len=%u", (unsigned) offset, (unsigned) io_len);

  if (enum_val(io_len) > 1)
    BX_PANIC("bad length!  page 2 read from port %04x, len=%u", (unsigned) offset, (unsigned) io_len);

  switch (offset) {
  case 0x1:  // PSTART
    return (BX_NE2K_THIS s.page_start);
    break;

  case 0x2:  // PSTOP
    return (BX_NE2K_THIS s.page_stop);
    break;

  case 0x3:  // Remote Next-packet pointer
    return (BX_NE2K_THIS s.rempkt_ptr);
    break;

  case 0x4:  // TPSR
    return (BX_NE2K_THIS s.tx_page_start);
    break;

  case 0x5:  // Local Next-packet pointer
    return (BX_NE2K_THIS s.localpkt_ptr);
    break;

  case 0x6:  // Address counter (upper)
    return (unsigned int)(BX_NE2K_THIS s.address_cnt >> 8u);
    break;

  case 0x7:  // Address counter (lower)
    return (unsigned int)(BX_NE2K_THIS s.address_cnt & 0xff);
    break;

  case 0x8:  // Reserved
  case 0x9:
  case 0xa:
  case 0xb:
	  BX_ERROR("NE2000: reserved read - page 2, 0x%02x", (unsigned)offset);
	  return (0xff);
	  break;

  case 0xc:  // RCR
    return
       ((unsigned int)(BX_NE2K_THIS s.RCR.monitor   << 5u) |
	    (unsigned int)(BX_NE2K_THIS s.RCR.promisc   << 4u) |
	    (unsigned int)(BX_NE2K_THIS s.RCR.multicast << 3u) |
	    (unsigned int)(BX_NE2K_THIS s.RCR.broadcast << 2u) |
	    (unsigned int)(BX_NE2K_THIS s.RCR.runts_ok  << 1u) |
	    (unsigned int)(BX_NE2K_THIS s.RCR.errors_ok));
    break;

  case 0xd:  // TCR
	  return
     ((unsigned int)(BX_NE2K_THIS s.TCR.coll_prio << 4u) |
		  (unsigned int)(BX_NE2K_THIS s.TCR.ext_stoptx << 3u) |
		               ((BX_NE2K_THIS s.TCR.loop_cntl & 0x3u) << 1u) |
		  (unsigned int)(BX_NE2K_THIS s.TCR.crc_disable));
	  break;

  case 0xe:  // DCR
    return
      (((unsigned int)(BX_NE2K_THIS s.DCR.fifo_size & 0x3) << 5u) |
	    (unsigned int)(BX_NE2K_THIS s.DCR.auto_rx          << 4u) |
	    (unsigned int)(BX_NE2K_THIS s.DCR.loop             << 3u) |
	    (unsigned int)(BX_NE2K_THIS s.DCR.longaddr         << 2u) |
	    (unsigned int)(BX_NE2K_THIS s.DCR.endian           << 1u) |
	    (unsigned int)(BX_NE2K_THIS s.DCR.wdsize));
    break;

  case 0xf:  // IMR
    return
       ((unsigned int)(BX_NE2K_THIS s.IMR.rdma_inte  << 6u) |
	    (unsigned int)(BX_NE2K_THIS s.IMR.cofl_inte  << 5u) |
	    (unsigned int)(BX_NE2K_THIS s.IMR.overw_inte << 4u) |
	    (unsigned int)(BX_NE2K_THIS s.IMR.txerr_inte << 3u) |
	    (unsigned int)(BX_NE2K_THIS s.IMR.rxerr_inte << 2u) |
	    (unsigned int)(BX_NE2K_THIS s.IMR.tx_inte    << 1u) |
	   (unsigned int) (BX_NE2K_THIS s.IMR.rx_inte));
    break;

  default:
    BX_PANIC("page 2 offset %04x out of range", (unsigned) offset);
  }

  return (0);
}

void
bx_ne2k_c::page2_write(io_port_t offset, io_val_t data, io_width_t io_len)
{
    (void)io_len;//UNUSED

  const auto value = check_cast<uint8_t>(data);
  // Maybe all writes here should be BX_PANIC()'d, since they
  // affect internal operation, but let them through for now
  // and print a warning.
  if (offset != 0)
	  BX_ERROR(("NE2000: page 2 write ?"));

  switch (offset) {
  case 0x1:  // CLDA0
    // Clear out low byte and re-insert
    BX_NE2K_THIS s.local_dma &= 0xff00;
    BX_NE2K_THIS s.local_dma |= (value & 0xff);
    break;

  case 0x2:  // CLDA1
    // Clear out high byte and re-insert
    BX_NE2K_THIS s.local_dma &= 0x00ff;
    BX_NE2K_THIS s.local_dma |= ((value & 0xff) << 8u);
    break;

  case 0x3:  // Remote Next-pkt pointer
    BX_NE2K_THIS s.rempkt_ptr = value;
    break;

  case 0x4:
    BX_PANIC(("page 2 write to reserved offset 4"));
    break;

  case 0x5:  // Local Next-packet pointer
    BX_NE2K_THIS s.localpkt_ptr = value;
    break;

  case 0x6:  // Address counter (upper)
    // Clear out high byte and re-insert
    BX_NE2K_THIS s.address_cnt &= 0x00ff;
    BX_NE2K_THIS s.address_cnt |= ((value & 0xff) << 8);
    break;

  case 0x7:  // Address counter (lower)
    // Clear out low byte and re-insert
    BX_NE2K_THIS s.address_cnt &= 0xff00;
    BX_NE2K_THIS s.address_cnt |= (value & 0xff);
    break;

  case 0x8:
  case 0x9:
  case 0xa:
  case 0xb:
  case 0xc:
  case 0xd:
  case 0xe:
  case 0xf:
    BX_PANIC("page 2 write to reserved offset %0x", offset);
    break;

  default:
    BX_PANIC("page 2 write, illegal offset %0x", offset);
    break;
  }
}

//
// page3_read/page3_write - writes to this page are illegal
//
uint32_t
bx_ne2k_c::page3_read(io_port_t, io_width_t)
{
  BX_PANIC(("page 3 read attempted"));
  return (0);
}

void
bx_ne2k_c::page3_write(io_port_t, io_val_t, io_width_t)
{
  BX_PANIC(("page 3 write attempted"));
}

//
// tx_timer_handler/tx_timer
//
void
bx_ne2k_c::tx_timer_handler(void *this_ptr)
{
  bx_ne2k_c *class_ptr = (bx_ne2k_c *) this_ptr;

  class_ptr->tx_timer();
}

void
bx_ne2k_c::tx_timer(void)
{
  BX_DEBUG(("tx_timer"));
  BX_NE2K_THIS s.TSR.tx_ok = 1;
  // Generate an interrupt if not masked and not one in progress
  if (BX_NE2K_THIS s.IMR.tx_inte && !BX_NE2K_THIS s.ISR.pkt_tx) {
		//LOG_MSG("tx complete interrupt");
	  PIC_ActivateIRQ(s.base_irq);
    //DEV_pic_raise_irq(BX_NE2K_THIS s.base_irq);
  } //else 	  LOG_MSG("no tx complete interrupt");
  BX_NE2K_THIS s.ISR.pkt_tx = 1;
  BX_NE2K_THIS s.tx_timer_active = 0;
}


//
// read_handler/read - i/o 'catcher' function called from BOCHS
// mainline when the CPU attempts a read in the i/o space registered
// by this ne2000 instance
//
io_val_t bx_ne2k_c::read_handler(void *this_ptr, io_port_t address, io_width_t io_len)
{
#if !BX_USE_NE2K_SMF
  bx_ne2k_c *class_ptr = (bx_ne2k_c *) this_ptr;

  return( class_ptr->read(address, io_len) );
}

io_val_t bx_ne2k_c::read(io_port_t address, io_width_t io_len)
{
#else
  UNUSED(this_ptr);
#endif  // !BX_USE_NE2K_SMF
  BX_DEBUG("read addr %x, len %d", address, io_len);
  uint32_t retval = 0;
  const io_port_t offset = address - (BX_NE2K_THIS s.base_address);

  if (offset >= 0x10) {
    retval = asic_read(offset - 0x10, io_len);
  } else if (offset == 0x00) {
    retval = read_cr();
  } else {
    switch (BX_NE2K_THIS s.CR.pgsel) {
    case 0x00:
      retval = page0_read(offset, io_len);
      break;

    case 0x01:
      retval = page1_read(offset, io_len);
      break;

    case 0x02:
      retval = page2_read(offset, io_len);
      break;

    case 0x03:
      retval = page3_read(offset, io_len);
      break;

    default:
      BX_PANIC("ne2K: unknown value of pgsel in read - %d",
	       BX_NE2K_THIS s.CR.pgsel);
    }
  }

  return (retval);
}

//
// write_handler/write - i/o 'catcher' function called from BOCHS
// mainline when the CPU attempts a write in the i/o space registered
// by this ne2000 instance
//
void
bx_ne2k_c::write_handler(void *this_ptr, io_port_t address, io_val_t value,
			 io_width_t io_len)
{
#if !BX_USE_NE2K_SMF
  bx_ne2k_c *class_ptr = (bx_ne2k_c *) this_ptr;

  class_ptr->write(address, value, io_len);
}

void
bx_ne2k_c::write(io_port_t address, io_val_t value, io_width_t io_len)
{
#else
  UNUSED(this_ptr);
#endif  // !BX_USE_NE2K_SMF
  BX_DEBUG("write with length %d", io_len);
  const io_port_t offset = address - (BX_NE2K_THIS s.base_address);

  //
  // The high 16 bytes of i/o space are for the ne2000 asic -
  //  the low 16 bytes are for the DS8390, with the current
  //  page being selected by the PS0,PS1 registers in the
  //  command register
  //
  if (offset >= 0x10) {
    asic_write(offset - 0x10, value, io_len);
	} else if (offset == 0x00) {
    write_cr(value);
  } else {
    switch (BX_NE2K_THIS s.CR.pgsel) {
    case 0x00:
      page0_write(offset, value, io_len);
      break;

    case 0x01:
      page1_write(offset, value, io_len);
      break;

    case 0x02:
      page2_write(offset, value, io_len);
      break;

    case 0x03:
      page3_write(offset, value, io_len);
      break;

    default:
      BX_PANIC("ne2K: unknown value of pgsel in write - %d",
	       BX_NE2K_THIS s.CR.pgsel);
    }
  }
}


/*
 * mcast_index() - return the 6-bit index into the multicast
 * table. Stolen unashamedly from FreeBSD's if_ed.c
 */
unsigned
bx_ne2k_c::mcast_index(const void *dst)
{
#define POLYNOMIAL 0x04c11db6
  unsigned long crc = 0xffffffffL;
  int carry, i, j;
  uint8_t b;
  const auto *ep = (const uint8_t *) dst;

  for (i = 6; --i >= 0;) {
      b = *ep++;
      for (j = 8; --j >= 0;) {
          carry = ((crc & 0x80000000L) ? 1 : 0) ^ (b & 0x01);
          crc <<= 1;
          b >>= 1;
          if (carry)
              crc = ((crc ^ POLYNOMIAL) | (unsigned int)carry);
      }
  }

  return (uint32_t)((crc & 0xfffffffful) >> 26ul); /* WARNING: Caller directly uses our 6-bit return as index. If not truncated, will cause a segfault */
#undef POLYNOMIAL
}

/*
 * Callback from the eth system driver when a frame has arrived
 */
/*
void
bx_ne2k_c::rx_handler(void *arg, const void *buf, unsigned len)
{
    // BX_DEBUG(("rx_handler with length %d", len));
  bx_ne2k_c *class_ptr = (bx_ne2k_c *) arg;
  if(
  class_ptr->rx_frame(buf, len);
}
*/
/*
 * rx_frame() - called by the platform-specific code when an
 * ethernet frame has been received. The destination address
 * is tested to see if it should be accepted, and if the
 * rx ring has enough room, it is copied into it and
 * the receive process is updated
 */
int bx_ne2k_c::rx_frame(const void *buf, unsigned io_len)
{
  int pages;
  int avail;
  unsigned idx;
//  int wrapped;
  uint8_t nextpage;
  uint8_t pkthdr[4];
  const auto *pktbuf = (const uint8_t *) buf;
  uint8_t *startptr;
  static uint8_t bcast_addr[6] = {0xff,0xff,0xff,0xff,0xff,0xff};

  if(io_len != 60) {
	BX_DEBUG("rx_frame with length %d", io_len);
  }

	//LOG_MSG("stop=%d, pagestart=%x, dcr_loop=%x, tcr_loopcntl=%x",
	//	BX_NE2K_THIS s.CR.stop, BX_NE2K_THIS s.page_start,
	//	BX_NE2K_THIS s.DCR.loop, BX_NE2K_THIS s.TCR.loop_cntl);
  if ((BX_NE2K_THIS s.CR.stop != 0) ||
      (BX_NE2K_THIS s.page_start == 0) /*||
      ((BX_NE2K_THIS s.DCR.loop == 0) &&
       (BX_NE2K_THIS s.TCR.loop_cntl != 0))*/) {
	  return -1;
  }

  // Add the pkt header + CRC to the length, and work
  // out how many 256-byte pages the frame would occupy
  pages = (int)((io_len + 4u + 4u + 255u)/256u);

  if (BX_NE2K_THIS s.curr_page < BX_NE2K_THIS s.bound_ptr) {
    avail = BX_NE2K_THIS s.bound_ptr - BX_NE2K_THIS s.curr_page;
  } else {
    avail = (BX_NE2K_THIS s.page_stop - BX_NE2K_THIS s.page_start) -
      (BX_NE2K_THIS s.curr_page - BX_NE2K_THIS s.bound_ptr);
//    wrapped = 1;
  }

  // Avoid getting into a buffer overflow condition by not attempting
  // to do partial receives. The emulation to handle this condition
  // seems particularly painful.
  if ((avail < pages)
#if BX_NE2K_NEVER_FULL_RING
      || (avail == pages)
#endif
      ) {
	BX_DEBUG("no space");
	return -1;
  }

  if ((io_len < 40/*60*/) && !BX_NE2K_THIS s.RCR.runts_ok) {
    BX_DEBUG("rejected small packet, length %d", io_len);
    return -1;
  }
  // some computers don't care...
  if (io_len < 60) io_len=60;

  // Do address filtering if not in promiscuous mode
  if (! BX_NE2K_THIS s.RCR.promisc) {
    if (!memcmp(buf, bcast_addr, 6)) {
      if (!BX_NE2K_THIS s.RCR.broadcast) {
	      return -1;
      }
    } else if (pktbuf[0] & 0x01) {
	if (! BX_NE2K_THIS s.RCR.multicast) {
		return -1;
	}
      idx = mcast_index(buf);
      if (!(BX_NE2K_THIS s.mchash[idx >> 3] & (1 << (idx & 0x7)))) {
	      return -1;
      }
    } else if (0 != memcmp(buf, BX_NE2K_THIS s.physaddr, 6)) {
	    return -1;
    }
  } else {
      BX_DEBUG(("rx_frame promiscuous receive"));
  }

    BX_INFO("rx_frame %d to %x:%x:%x:%x:%x:%x from %x:%x:%x:%x:%x:%x",
  	   io_len,
  	   pktbuf[0], pktbuf[1], pktbuf[2], pktbuf[3], pktbuf[4], pktbuf[5],
  	   pktbuf[6], pktbuf[7], pktbuf[8], pktbuf[9], pktbuf[10], pktbuf[11]);

  nextpage = check_cast<uint8_t>(BX_NE2K_THIS s.curr_page + pages);
  if (nextpage >= BX_NE2K_THIS s.page_stop) {
    nextpage -= check_cast<uint8_t>(BX_NE2K_THIS s.page_stop - BX_NE2K_THIS s.page_start);
  }

  // Setup packet header
  // pkthdr[0] = 0;	// rx status - old behavior
  pkthdr[0] = 1;        // Probably better to set it all the time
                        // rather than set it to 0, which is clearly wrong.
  if (pktbuf[0] & 0x01) {
    pkthdr[0] |= 0x20;  // rx status += multicast packet
  }
  pkthdr[1] = nextpage;	// ptr to next packet
  pkthdr[2] = (io_len + 4) & 0xff;	// length-low
  pkthdr[3] = check_cast<uint8_t>((io_len + 4) >> 8);	// length-hi

  // copy into buffer, update curpage, and signal interrupt if config'd
  startptr = & BX_NE2K_THIS s.mem[BX_NE2K_THIS s.curr_page * 256 -
			       BX_NE2K_MEMSTART];
  if ((nextpage > BX_NE2K_THIS s.curr_page) ||
      ((BX_NE2K_THIS s.curr_page + pages) == BX_NE2K_THIS s.page_stop)) {
    memcpy(startptr, pkthdr, 4);
    memcpy(startptr + 4, buf, io_len);
    BX_NE2K_THIS s.curr_page = nextpage;
  } else {
    unsigned int endbytes = (unsigned int)(BX_NE2K_THIS s.page_stop - BX_NE2K_THIS s.curr_page)
      * 256u;
    memcpy(startptr, pkthdr, 4);
    memcpy(startptr + 4, buf, (size_t)(endbytes - 4u));
    startptr = & BX_NE2K_THIS s.mem[BX_NE2K_THIS s.page_start * 256u -
				 BX_NE2K_MEMSTART];
    memcpy(startptr, (const void *)(pktbuf + endbytes - 4u),
	   io_len - endbytes + 8u);
    BX_NE2K_THIS s.curr_page = nextpage;
  }

  BX_NE2K_THIS s.RSR.rx_ok = 1;
  if (pktbuf[0] & 0x80) {
    BX_NE2K_THIS s.RSR.rx_mbit = 1;
  }

  BX_NE2K_THIS s.ISR.pkt_rx = 1;

  if (BX_NE2K_THIS s.IMR.rx_inte) {
	//LOG_MSG("packet rx interrupt");
	  PIC_ActivateIRQ(s.base_irq);
    //DEV_pic_raise_irq(BX_NE2K_THIS s.base_irq);
  } //else LOG_MSG("no packet rx interrupt");
  return static_cast<int>(io_len);
}

//uint8_t macaddr[6] = { 0xAC, 0xDE, 0x48, 0x8E, 0x89, 0x19 };

io_val_t dosbox_read(io_port_t port, io_width_t width)
{
	const auto retval = theNE2kDevice->read(port,width);
	// LOG_MSG("ne2k rd port %x val %4x len %d page %d, CS:IP %8x:%8x",
	//	port, retval, len, theNE2kDevice->s.CR.pgsel,SegValue(cs),reg_eip);
	return retval;
}
void dosbox_write(io_port_t port, io_val_t value, io_width_t width)
{
  const auto val = check_cast<uint16_t>(value);
	// LOG_MSG("ne2k wr port %x val %4x len %d page %d, CS:IP %8x:%8x",
	//	port, val, len,theNE2kDevice->s.CR.pgsel,SegValue(cs),reg_eip);
	theNE2kDevice->write(port, val, width);
}

void bx_ne2k_c::init()
{
  //BX_DEBUG(("Init $Id: ne2k.cc,v 1.56.2.1 2004/02/02 22:37:22 cbothamy Exp $"));

  // Read in values from config file
  //BX_NE2K_THIS s.base_address = 0x300;
  //BX_NE2K_THIS s.base_irq     = 3;
  /*
  if (BX_NE2K_THIS s.tx_timer_index == BX_NULL_TIMER_HANDLE) {
    BX_NE2K_THIS s.tx_timer_index =
      bx_pc_system.register_timer(this, tx_timer_handler, 0,
                                  0,0, "ne2k"); // one-shot, inactive
  }*/
  // Register the IRQ and i/o port addresses
  //DEV_register_irq(BX_NE2K_THIS s.base_irq, "NE2000 ethernet NIC");

   //DEV_register_ioread_handler(this, read_handler, addr, "ne2000 NIC", 3);
   //DEV_register_iowrite_handler(this, write_handler, addr, "ne2000 NIC", 3);


  BX_INFO("port 0x%x/32 irq %d mac %02x:%02x:%02x:%02x:%02x:%02x",
           (BX_NE2K_THIS s.base_address),
           (int)(BX_NE2K_THIS s.base_irq),
           BX_NE2K_THIS s.physaddr[0],
           BX_NE2K_THIS s.physaddr[1],
           BX_NE2K_THIS s.physaddr[2],
           BX_NE2K_THIS s.physaddr[3],
           BX_NE2K_THIS s.physaddr[4],
           BX_NE2K_THIS s.physaddr[5]);

  // Initialise the mac address area by doubling the physical address
  BX_NE2K_THIS s.macaddr[0]  = BX_NE2K_THIS s.physaddr[0];
  BX_NE2K_THIS s.macaddr[1]  = BX_NE2K_THIS s.physaddr[0];
  BX_NE2K_THIS s.macaddr[2]  = BX_NE2K_THIS s.physaddr[1];
  BX_NE2K_THIS s.macaddr[3]  = BX_NE2K_THIS s.physaddr[1];
  BX_NE2K_THIS s.macaddr[4]  = BX_NE2K_THIS s.physaddr[2];
  BX_NE2K_THIS s.macaddr[5]  = BX_NE2K_THIS s.physaddr[2];
  BX_NE2K_THIS s.macaddr[6]  = BX_NE2K_THIS s.physaddr[3];
  BX_NE2K_THIS s.macaddr[7]  = BX_NE2K_THIS s.physaddr[3];
  BX_NE2K_THIS s.macaddr[8]  = BX_NE2K_THIS s.physaddr[4];
  BX_NE2K_THIS s.macaddr[9]  = BX_NE2K_THIS s.physaddr[4];
  BX_NE2K_THIS s.macaddr[10] = BX_NE2K_THIS s.physaddr[5];
  BX_NE2K_THIS s.macaddr[11] = BX_NE2K_THIS s.physaddr[5];

  // ne2k signature
  for (Bitu i = 12; i < 32; i++)
    BX_NE2K_THIS s.macaddr[i] = 0x57;

  // Bring the register state into power-up state
  reset(BX_RESET_HARDWARE);
}

static void NE2000_TX_Event([[maybe_unused]] uint32_t val)
{
	theNE2kDevice->tx_timer();
}

static void NE2000_Poller(void) {
	ethernet->GetPackets([](const uint8_t *packet, int len) {
		//LOG_MSG("NE2000: Received %d bytes", header->len);

		// don't receive in loopback modes
		if((theNE2kDevice->s.DCR.loop == 0) || (theNE2kDevice->s.TCR.loop_cntl != 0))
			return -1;
		return theNE2kDevice->rx_frame(packet, check_cast<uint16_t>(len));
	});
}

class NE2K final : public ModuleBase {
private:
	// Data
	IO_ReadHandleObject ReadHandler8[0x20];
	IO_WriteHandleObject WriteHandler8[0x20];
	IO_ReadHandleObject ReadHandler16[0x10];
	IO_WriteHandleObject WriteHandler16[0x10];

public:
	bool load_success;
	NE2K(Section *configuration)
	        : ModuleBase(configuration),
	          load_success(true)
	{
		SectionProp *section = static_cast<SectionProp *>(configuration);
		if (!section->GetBool("ne2000")) {
			load_success = false;
			return;
		}

		ethernet = ETHERNET_OpenConnection("slirp");
		if (!ethernet) {
			LOG_WARNING(
			        "NE2000: Failed to initialise Slirp Ethernet backend; "
			        "setting 'ne2000' to 'off'");

			set_section_property_value("ethernet", "ne2000", "off");
			load_success = false;
			return;
		}

		// get irq and base
		auto irq = check_cast<uint8_t>(section->GetInt("nicirq"));
		if(!(irq==3 || irq==4  || irq==5  || irq==6 ||irq==7 ||
			irq==9 || irq==10 || irq==11 || irq==12 ||irq==14 ||irq==15)) {
			irq=3;
		}
		auto base = static_cast<io_port_t>(section->GetHex("nicbase"));
		if(!(base==0x260||base==0x280||base==0x300||base==0x320||base==0x340||base==0x380)) {
			base=0x300;
		}

        LOG_MSG("NE2000: Initialised on port %xh and IRQ %u", base, irq);

		// mac address
		std::string macstring = section->GetString("macaddr");
		unsigned int macint[6];
		uint8_t mac[6];
		if(sscanf(macstring.c_str(),"%02x:%02x:%02x:%02x:%02x:%02x",
			&macint[0],&macint[1],&macint[2],&macint[3],&macint[4],&macint[5]) != 6) {
			mac[0]=0xac;mac[1]=0xde;mac[2]=0x48;
			mac[3]=0x88;mac[4]=0xbb;mac[5]=0xaa;
		} else {
			mac[0] = check_cast<uint8_t>(macint[0]);
      mac[1] = check_cast<uint8_t>(macint[1]);
			mac[2] = check_cast<uint8_t>(macint[2]);
      mac[3] = check_cast<uint8_t>(macint[3]);
			mac[4] = check_cast<uint8_t>(macint[4]);
      mac[5] = check_cast<uint8_t>(macint[5]);
		}

		// create the bochs NIC class
		theNE2kDevice = new bx_ne2k_c ();
		memcpy(theNE2kDevice->s.physaddr, mac, 6);

		theNE2kDevice->s.base_address = base;
		theNE2kDevice->s.base_irq = irq;

		theNE2kDevice->init();

		// install I/O-handlers and timer
		for(io_port_t i = 0; i < 0x20; ++i) {
      const auto port_num = static_cast<io_port_t>(i + theNE2kDevice->s.base_address);
			ReadHandler8[i].Install(port_num, dosbox_read, io_width_t::word);
			WriteHandler8[i].Install(port_num, dosbox_write, io_width_t::word);
		}
		TIMER_AddTickHandler(NE2000_Poller);
	}

	~NE2K() {
		delete ethernet;
		ethernet = nullptr;
		delete theNE2kDevice;
		theNE2kDevice = nullptr;
		TIMER_DelTickHandler(NE2000_Poller);
		PIC_RemoveEvents(NE2000_TX_Event);
	}
};

static NE2K* instance;
void NE2K_ShutDown(Section* /* sec */)
{
	delete instance;
	instance = nullptr;
}

void NE2K_Init(Section* sec)
{
	assert(sec);
	// LOG(LOG_MISC,LOG_DEBUG)("Initializing NE2000 network card emulation");

	instance = new NE2K(sec);

	constexpr auto changeable_at_runtime = true;
	sec->AddDestroyFunction(&NE2K_ShutDown, changeable_at_runtime);

	if (!instance->load_success) {
		delete instance;
		instance = nullptr;
	}
}

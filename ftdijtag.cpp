#include <libusb.h>

#include <iostream>
#include <map>
#include <vector>
#include <stdio.h>
#include <string.h>
#include <string>

#include "ftdijtag.hpp"
#include "ftdipp_mpsse.hpp"

using namespace std;

#define DEBUG 0

#ifdef DEBUG
#define display(...) \
	do { if (_verbose) fprintf(stdout, __VA_ARGS__);}while(0)
#else
#define display(...) do {}while(0)
#endif

/*
 * AD0 -> TCK
 * AD1 -> TDI
 * AD2 -> TD0
 * AD3 -> TMS
 */

/* Rmq:
 * pour TMS: l'envoi de n necessite de mettre n-1 comme longueur
 *           mais le bit n+1 est utilise pour l'etat suivant le dernier
 *           front. Donc il faut envoyer 6bits ([5:0]) pertinents pour
 *           utiliser le bit 6 comme etat apres la commande,
 *           le bit 7 corresponds a l'etat de TDI (donc si on fait 7 cycles
 *           l'etat de TDI va donner l'etat de TMS...)
 * transfert/lecture: le dernier bit de IR ou DR doit etre envoye en
 *           meme temps que le TMS qui fait sortir de l'etat donc il faut
 *           pour n bits a transferer :
 *           - envoyer 8bits * (n/8)-1
 *           - envoyer les 7 bits du dernier octet;
 *           - envoyer le dernier avec 0x4B ou 0x6B
 */

FtdiJtag::FtdiJtag(FTDIpp_MPSSE::mpsse_bit_config &cable, string dev,
			unsigned char interface, uint32_t clkHZ, bool verbose):
			FTDIpp_MPSSE(dev, interface, clkHZ, verbose),
			_state(RUN_TEST_IDLE),
			_tms_buffer_size(128), _num_tms(0),
			_board_name("nope"), _ch552WA(false)
{
	init_internal(cable);
}

FtdiJtag::FtdiJtag(FTDIpp_MPSSE::mpsse_bit_config &cable,
		   unsigned char interface, uint32_t clkHZ, bool verbose):
		   FTDIpp_MPSSE(cable.vid, cable.pid, interface, clkHZ, verbose),
		   _state(RUN_TEST_IDLE),
		   _tms_buffer_size(128), _num_tms(0),
		   _board_name("nope"), _ch552WA(false)
{
	init_internal(cable);
}

FtdiJtag::~FtdiJtag()
{
	int read;
	/* Before shutdown, we must wait until everything is shifted out
	 * Do this by temporary enabling loopback mode, write something
	 * and wait until we can read it back
	 * */
	static unsigned char tbuf[16] = { SET_BITS_LOW, 0xff, 0x00,
		SET_BITS_HIGH, 0xff, 0x00,
		LOOPBACK_START,
		MPSSE_DO_READ |
		MPSSE_DO_WRITE | MPSSE_WRITE_NEG | MPSSE_LSB,
		0x04, 0x00,
		0xaa, 0x55, 0x00, 0xff, 0xaa,
		LOOPBACK_END
	};
	mpsse_store(tbuf, 16);
	read = mpsse_read(tbuf, 5);
	if (read != 5)
		fprintf(stderr,
			"Loopback failed, expect problems on later runs %d\n", read);

	free(_tms_buffer);
}

void FtdiJtag::init_internal(FTDIpp_MPSSE::mpsse_bit_config &cable)
{
	/* search for iProduct -> need to have
	 * ftdi->usb_dev (libusb_device_handler) -> libusb_device ->
	 * libusb_device_descriptor
	 */
	struct libusb_device * usb_dev = libusb_get_device(_ftdi->usb_dev);
	struct libusb_device_descriptor usb_desc;
	unsigned char iProduct[200];
	libusb_get_device_descriptor(usb_dev, &usb_desc);
	libusb_get_string_descriptor_ascii(_ftdi->usb_dev, usb_desc.iProduct,
		iProduct, 200);

	display("iProduct : %s\n", iProduct);
	if (!strncmp((const char *)iProduct, "Sipeed-Debug", 12)) {
		_ch552WA = true;
	}

	display("board_name %s\n", _board_name.c_str());
	display("%x\n", cable.bit_low_val);
	display("%x\n", cable.bit_low_dir);
	display("%x\n", cable.bit_high_val);
	display("%x\n", cable.bit_high_dir);

	_tms_buffer = (unsigned char *)malloc(sizeof(unsigned char) * _tms_buffer_size);
	bzero(_tms_buffer, _tms_buffer_size);
	init(5, 0xfb, cable);
}

int FtdiJtag::detectChain(vector<int> &devices, int max_dev)
{
	unsigned char rx_buff[4];
	/* WA for CH552/tangNano: write is always mandatory */
	unsigned char tx_buff[4] = {0xff, 0xff, 0xff, 0xff};
	unsigned int tmp;

	devices.clear();
	go_test_logic_reset();
	set_state(SHIFT_DR);

	for (int i = 0; i < max_dev; i++) {
		read_write(tx_buff, rx_buff, 32, (i == max_dev-1)?1:0);
		tmp = 0;
		for (int ii=0; ii < 4; ii++)
			tmp |= (rx_buff[ii] << (8*ii));
		if (tmp != 0 && tmp != 0xffffffff)
			devices.push_back(tmp);
	}
	go_test_logic_reset();
	return devices.size();
}

void FtdiJtag::setTMS(unsigned char tms)
{
	display("%s %d %d\n", __func__, _num_tms, (_num_tms >> 3));
	if (_num_tms+1 == _tms_buffer_size * 8)
		flushTMS();
	if (tms != 0)
		_tms_buffer[_num_tms>>3] |= (0x1) << (_num_tms & 0x7);
	_num_tms++;
}

/* reconstruct byte sent to TMS pins
 * - use up to 6 bits
 * -since next bit after length is use to
 *  fix TMS state after sent we copy last bit
 *  to bit after next
 * -bit 7 is TDI state for each clk cycles
 */

int FtdiJtag::flushTMS(bool flush_buffer)
{
	int xfer, pos = 0;
	unsigned char buf[3]= {MPSSE_WRITE_TMS | MPSSE_LSB | MPSSE_BITMODE |
			MPSSE_WRITE_NEG, 0, 0};

	if (_num_tms == 0)
		return 0;

	display("%s: %d %x\n", __func__, _num_tms, _tms_buffer[0]);

	while (_num_tms != 0) {
		xfer = (_num_tms > 6) ? 6 : _num_tms;
		buf[1] = xfer - 1;
		buf[2] = 0x80;
		for (int i = 0; i < xfer; i++, pos++) {
			buf[2] |=
			(((_tms_buffer[pos >> 3] & (1 << (pos & 0x07))) ? 1 : 0) << i);
		}
		_num_tms -= xfer;
		mpsse_store(buf, 3);
	}

	/* reset buffer and number of bits */
	bzero(_tms_buffer, _tms_buffer_size);
	_num_tms = 0;
	if (flush_buffer)
		return mpsse_write();
	return 0;
}

void FtdiJtag::go_test_logic_reset()
{
	/* idenpendly to current state 5 clk with TMS high is enough */
	for (int i = 0; i < 6; i++)
		setTMS(0x01);
	flushTMS(true);
	_state = TEST_LOGIC_RESET;
}

/* GGM: faut tenir plus compte de la taille de la fifo interne
 *      du FT2232 pour maximiser l'envoi au lieu de faire de petits envoies
 */
int FtdiJtag::read_write(unsigned char *tdi, unsigned char *tdo, int len, char last)
{
	/* 3 possible case :
	 *  - n * 8bits to send -> use byte command
	 *  - less than 8bits   -> use bit command
	 *  - last bit to send  -> sent in conjunction with TMS
	 */
	int tx_buff_size = mpsse_get_buffer_size();
	int real_len = (last) ? len - 1 : len;	// if its a buffer in a big send send len
						// else supress last bit -> with TMS
	int nb_byte = real_len >> 3;	// number of byte to send
	int nb_bit = (real_len & 0x07);	// residual bits
	int xfer = tx_buff_size - 3;
	unsigned char c[len];
	unsigned char *rx_ptr = (unsigned char *)tdo;
	unsigned char *tx_ptr = (unsigned char *)tdi;
	unsigned char tx_buf[3] = {(unsigned char)(MPSSE_LSB | MPSSE_WRITE_NEG |
							   ((tdi) ? MPSSE_DO_WRITE : 0) |
							   ((tdo) ? MPSSE_DO_READ : 0)),
							   static_cast<unsigned char>((xfer - 1) & 0xff),		// low
							   static_cast<unsigned char>((((xfer - 1) >> 8) & 0xff))};	// high

	flushTMS(true);

	display("%s len : %d %d %d %d\n", __func__, len, real_len, nb_byte,
		nb_bit);
	while (nb_byte > xfer) {
		mpsse_store(tx_buf, 3);
		if (tdi) {
			mpsse_store(tx_ptr, xfer);
			tx_ptr += xfer;
		}
		if (tdo) {
			mpsse_read(rx_ptr, xfer);
			rx_ptr += xfer;
		} else if (_ch552WA) {
			ftdi_read_data(_ftdi, c, xfer);
		}
		nb_byte -= xfer;
	}


	/* 1/ send serie of byte */
	if (nb_byte > 0) {
		display("%s read/write %d byte\n", __func__, nb_byte);
		tx_buf[1] = ((nb_byte - 1) & 0xff);		// low
		tx_buf[2] = (((nb_byte - 1) >> 8) & 0xff);	// high
		mpsse_store(tx_buf, 3);
		if (tdi) {
			mpsse_store(tx_ptr, nb_byte);
			tx_ptr += nb_byte;
		}
		if (tdo) {
			mpsse_read(rx_ptr, nb_byte);
			rx_ptr += nb_byte;
		} else if (_ch552WA) {
			ftdi_read_data(_ftdi, c, nb_byte);
		}
	}

	unsigned char last_bit = (tdi) ? *tx_ptr : 0;

	if (nb_bit != 0) {
		display("%s read/write %d bit\n", __func__, nb_bit);
		tx_buf[0] |= MPSSE_BITMODE;
		tx_buf[1] = nb_bit - 1;
		mpsse_store(tx_buf, 2);
		if (tdi) {
			display("%s last_bit %x size %d\n", __func__, last_bit, nb_bit-1);
			mpsse_store(last_bit);
		}
		mpsse_write();
		if (tdo) {
			mpsse_read(rx_ptr, 1);
			/* realign we have read nb_bit 
			 * since LSB add bit by the left and shift
			 * we need to complete shift
			 */
			*rx_ptr >>= (8 - nb_bit);
			display("%s %x\n", __func__, *rx_ptr);
		} else if (_ch552WA) {
			ftdi_read_data(_ftdi, c, nb_bit);
		}
	}

	/* display : must be dropped */
	if (_verbose && tdo) {
		display("\n");
		for (int i = (len / 8) - 1; i >= 0; i--)
			display("%x ", (unsigned char)tdo[i]);
		display("\n");
	}

	if (last == 1) {
		last_bit = (tdi)? (*tx_ptr & (1 << nb_bit)) : 0;

		display("%s move to EXIT1_xx and send last bit %x\n", __func__, (last_bit?0x81:0x01));
		/* write the last bit in conjunction with TMS */
		tx_buf[0] = MPSSE_WRITE_TMS | MPSSE_LSB | MPSSE_BITMODE | MPSSE_WRITE_NEG |
					((tdo) ? MPSSE_DO_READ : 0);
		tx_buf[1] = 0x0 ;	// send 1bit
		tx_buf[2] = ((last_bit)?0x81:0x01);	// we know in TMS tdi is bit 7
							// and to move to EXIT_XR TMS = 1
		mpsse_store(tx_buf, 3);
		mpsse_write();
		if (tdo) {
			unsigned char c;
			mpsse_read(&c, 1);
			/* in this case for 1 one it's always bit 7 */
			*rx_ptr |= ((c & 0x80) << (7 - nb_bit));
			display("%s %x\n", __func__, c);
		} else if (_ch552WA) {
			ftdi_read_data(_ftdi, c, 1);
		}
		_state = (_state == SHIFT_DR) ? EXIT1_DR : EXIT1_IR;
	}

	return 0;
}

void FtdiJtag::toggleClk(int nb)
{
	unsigned char c = (TEST_LOGIC_RESET == _state) ? 1 : 0;
	for (int i = 0; i < nb; i++)
		setTMS(c);
	flushTMS(true);
}

int FtdiJtag::shiftDR(unsigned char *tdi, unsigned char *tdo, int drlen, int end_state)
{
	set_state(SHIFT_DR);
	// force transmit tms state
	flushTMS(true);
	// currently don't care about multiple device in the chain
	read_write(tdi, tdo, drlen, 1);// 1 since only one device

	set_state(end_state);
	return 0;
}

int FtdiJtag::shiftIR(unsigned char tdi, int irlen, int end_state)
{
	if (irlen > 8) {
		cerr << "Error: this method this direct char don't support more than 1 byte" << endl;
		return -1;
	}
	return shiftIR(&tdi, NULL, irlen, end_state);
}

int FtdiJtag::shiftIR(unsigned char *tdi, unsigned char *tdo, int irlen, int end_state)
{
	display("%s: avant shiftIR\n", __func__);
	set_state(SHIFT_IR);
	flushTMS(true);
	// currently don't care about multiple device in the chain

	display("%s: envoi ircode\n", __func__);
	read_write(tdi, tdo, irlen, 1);// 1 since only one device

	set_state(end_state);
	return 0;
}

void FtdiJtag::set_state(int newState)
{
	unsigned char tms;
	while (newState != _state) {
		display("_state : %16s(%02d) -> %s(%02d) ",
			getStateName((tapState_t)_state),
			_state,
			getStateName((tapState_t)newState), newState);
		switch (_state) {
		case TEST_LOGIC_RESET:
			if (newState == TEST_LOGIC_RESET) {
				tms = 1;
			} else {
				tms = 0;
				_state = RUN_TEST_IDLE;
			}
			break;
		case RUN_TEST_IDLE:
			if (newState == RUN_TEST_IDLE) {
				tms = 0;
			} else {
				tms = 1;
				_state = SELECT_DR_SCAN;
			}
			break;
		case SELECT_DR_SCAN:
			switch (newState) {
			case CAPTURE_DR:
			case SHIFT_DR:
			case EXIT1_DR:
			case PAUSE_DR:
			case EXIT2_DR:
			case UPDATE_DR:
				tms = 0;
				_state = CAPTURE_DR;
				break;
			default:
				tms = 1;
				_state = SELECT_IR_SCAN;
			}
			break;
		case SELECT_IR_SCAN:
			switch (newState) {
			case CAPTURE_IR:
			case SHIFT_IR:
			case EXIT1_IR:
			case PAUSE_IR:
			case EXIT2_IR:
			case UPDATE_IR:
				tms = 0;
				_state = CAPTURE_IR;
				break;
			default:
				tms = 1;
				_state = TEST_LOGIC_RESET;
			}
			break;
			/* DR column */
		case CAPTURE_DR:
			if (newState == SHIFT_DR) {
				tms = 0;
				_state = SHIFT_DR;
			} else {
				tms = 1;
				_state = EXIT1_DR;
			}
			break;
		case SHIFT_DR:
			if (newState == SHIFT_DR) {
				tms = 0;
			} else {
				tms = 1;
				_state = EXIT1_DR;
			}
			break;
		case EXIT1_DR:
			switch (newState) {
			case PAUSE_DR:
			case EXIT2_DR:
			case SHIFT_DR:
			case EXIT1_DR:
				tms = 0;
				_state = PAUSE_DR;
				break;
			default:
				tms = 1;
				_state = UPDATE_DR;
			}
			break;
		case PAUSE_DR:
			if (newState == PAUSE_DR) {
				tms = 0;
			} else {
				tms = 1;
				_state = EXIT2_DR;
			}
			break;
		case EXIT2_DR:
			switch (newState) {
			case SHIFT_DR:
			case EXIT1_DR:
			case PAUSE_DR:
				tms = 0;
				_state = SHIFT_DR;
				break;
			default:
				tms = 1;
				_state = UPDATE_DR;
			}
			break;
		case UPDATE_DR:
			if (newState == RUN_TEST_IDLE) {
				tms = 0;
				_state = RUN_TEST_IDLE;
			} else {
				tms = 1;
				_state = SELECT_DR_SCAN;
			}
			break;
			/* IR column */
		case CAPTURE_IR:
			if (newState == SHIFT_IR) {
				tms = 0;
				_state = SHIFT_IR;
			} else {
				tms = 1;
				_state = EXIT1_IR;
			}
			break;
		case SHIFT_IR:
			if (newState == SHIFT_IR) {
				tms = 0;
			} else {
				tms = 1;
				_state = EXIT1_IR;
			}
			break;
		case EXIT1_IR:
			switch (newState) {
			case PAUSE_IR:
			case EXIT2_IR:
			case SHIFT_IR:
			case EXIT1_IR:
				tms = 0;
				_state = PAUSE_IR;
				break;
			default:
				tms = 1;
				_state = UPDATE_IR;
			}
			break;
		case PAUSE_IR:
			if (newState == PAUSE_IR) {
				tms = 0;
			} else {
				tms = 1;
				_state = EXIT2_IR;
			}
			break;
		case EXIT2_IR:
			switch (newState) {
			case SHIFT_IR:
			case EXIT1_IR:
			case PAUSE_IR:
				tms = 0;
				_state = SHIFT_IR;
				break;
			default:
				tms = 1;
				_state = UPDATE_IR;
			}
			break;
		case UPDATE_IR:
			if (newState == RUN_TEST_IDLE) {
				tms = 0;
				_state = RUN_TEST_IDLE;
			} else {
				tms = 1;
				_state = SELECT_DR_SCAN;
			}
			break;
		}

		setTMS(tms);
		display("%d %d %d %x\n", tms, _num_tms-1, _state, _tms_buffer[(_num_tms-1) / 8]);
	}
	/* force write buffer */
	flushTMS();
}

const char *FtdiJtag::getStateName(tapState_t s)
{
	switch (s) {
	case TEST_LOGIC_RESET:
		return "TEST_LOGIC_RESET";
	case RUN_TEST_IDLE:
		return "RUN_TEST_IDLE";
	case SELECT_DR_SCAN:
		return "SELECT_DR_SCAN";
	case CAPTURE_DR:
		return "CAPTURE_DR";
	case SHIFT_DR:
		return "SHIFT_DR";
	case EXIT1_DR:
		return "EXIT1_DR";
	case PAUSE_DR:
		return "PAUSE_DR";
	case EXIT2_DR:
		return "EXIT2_DR";
	case UPDATE_DR:
		return "UPDATE_DR";
	case SELECT_IR_SCAN:
		return "SELECT_IR_SCAN";
	case CAPTURE_IR:
		return "CAPTURE_IR";
	case SHIFT_IR:
		return "SHIFT_IR";
	case EXIT1_IR:
		return "EXIT1_IR";
	case PAUSE_IR:
		return "PAUSE_IR";
	case EXIT2_IR:
		return "EXIT2_IR";
	case UPDATE_IR:
		return "UPDATE_IR";
	default:
		return "Unknown";
	}
}

/*
 * ccid_serial.c: communicate with a GemPC Twin smart card reader
 * Copyright (C) 2001-2004 Ludovic Rousseau <ludovic.rousseau@free.fr>
 *
 * Thanks to Niki W. Waibel <niki.waibel@gmx.net> for a prototype version
 * 
    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */

/*
 * $Id$
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/types.h>
#include <PCSC/ifdhandler.h>

#include "defs.h"
#include "ccid_ifdhandler.h"
#include "config.h"
#include "debug.h"
#include "ccid.h"
#include "utils.h"
#include "commands.h"

#define SYNC 0x03
#define CTRL_ACK 0x06
#define CTRL_NAK 0x15
#define RDR_to_PC_NotifySlotChange 0x50
#define CARD_ABSENT 0x02
#define CARD_PRESENT 0x03

/*
 * normal command:
 * 1 : SYNC
 * 1 : CTRL
 * 10 +data length : CCID command
 * 1 : LRC
 *
 * SYNC : 0x03
 * CTRL : ACK (0x06) or NAK (0x15)
 * CCID command : see USB CCID specs
 * LRC : xor of all the previous byes
 *
 * Error message:
 * 1 : SYNC (0x03)
 * 1 : CTRL (NAK: 0x15)
 * 1 : LRC (0x16)
 *
 * Card insertion/withdrawal
 * 1 : RDR_to_PC_NotifySlotChange (0x50)
 * 1 : bmSlotIccState
 *     0x02 if card absent
 *     0x03 is card present
 *
 * Time request
 * T=1 : normal CCID command
 * T=0 : 1 byte (value between 0x80 and 0xFF)
 *
 */

/*
 * You may get read timeout after a card movement.
 * This is because you will get the echo of the CCID command
 * but not the result of the command.
 *
 * This is not an applicative issue since the card is either removed (and
 * powered off) or just inserted (and not yet powered on).
 */

/* 271 = max size for short APDU
 * 2 bytes for header
 * 1 byte checksum
 * doubled for echo
 */
#define GEMPCTWIN_MAXBUF (271 +2 +1) * 2

typedef struct
{
	/*
	 * File handle on the serial port
	 */
	int fd;

	/*
	 * device used ("/dev/ttyS?" under Linux)
	 */
	/*@null@*/ char *device;

	/*
	 * serial communication buffer
	 */
	unsigned char buffer[GEMPCTWIN_MAXBUF];

	/*
	 * next available byte
	 */
	int buffer_offset;

	/*
	 * number of available bytes
	 */
	int buffer_offset_last;

	/*
	 * CCID infos common to USB and serial
	 */
	_ccid_descriptor ccid;

} _serialDevice;

/* The _serialDevice structure must be defined before including ccid_serial.h */
#include "ccid_serial.h"

int SerialDataRates[] = {
		10753,
		14337,
		15625,
		17204,
		20833,
		21505,
		23438,
		25806,
		28674,
		31250,
		32258,
		34409,
		39063,
		41667,
		43011,
		46875,
		52083,
		53763,
		57348,
		62500,
		64516,
		68817,
		71685,
		78125,
		83333,
		86022,
		93750,
		104667,
		107527,
		114695,
		125000,
		129032,
		143369,
		156250,
		166667,
		172043,
		215054,
		229391,
		250000,
		344086,
		0
	};

/* no need to initialize to 0 since it is static */
static _serialDevice serialDevice[CCID_DRIVER_MAX_READERS];

/* unexported functions */
static int ReadChunk(unsigned int reader_index, unsigned char *buffer,
	int buffer_length, int min_length);

static int get_bytes(unsigned int reader_index, unsigned char *buffer,
	int length);


/*****************************************************************************
 * 
 *				WriteSerial: Send bytes to the card reader
 *
 *****************************************************************************/
status_t WriteSerial(unsigned int reader_index, unsigned int length,
	unsigned char *buffer)
{
	int i;
	unsigned char lrc;
	unsigned char low_level_buffer[GEMPCTWIN_MAXBUF];

#ifdef DEBUG_LEVEL_COMM
	char debug_header[] = "-> 123456 ";

	sprintf(debug_header, "-> %06X ", reader_index);
#endif

	if (length > GEMPCTWIN_MAXBUF-3)
	{
		DEBUG_CRITICAL3("command too long: %d for max %d",
			length, GEMPCTWIN_MAXBUF-3);
		return STATUS_UNSUCCESSFUL;
	}

	/* header */
	low_level_buffer[0] = 0x03;	/* SYNC */
	low_level_buffer[1] = 0x06;	/* ACK */

	/* CCID command */
	memcpy(low_level_buffer+2, buffer, length);

	/* checksum */
	lrc = 0;
	for(i=0; i<length+2; i++)
		lrc ^= low_level_buffer[i];
	low_level_buffer[length+2] = lrc;

#ifdef DEBUG_LEVEL_COMM
	DEBUG_XXD(debug_header, low_level_buffer, length+3);
#endif

	if (write(serialDevice[reader_index].fd, low_level_buffer,
		length+3) != length+3)
	{
		DEBUG_CRITICAL2("write error: %s", strerror(errno));
		return STATUS_UNSUCCESSFUL;
	}

	return STATUS_SUCCESS;
} /* WriteSerial */


/*****************************************************************************
 * 
 *				ReadSerial: Receive bytes from the card reader
 *
 *****************************************************************************/
status_t ReadSerial(unsigned int reader_index,
	/*@unused@*/ unsigned int *length, unsigned char *buffer)
{
	unsigned char c;
	int rv;
	int echo;
	int to_read;
	int i;

	/* we get the echo first */
	echo = TRUE;

start:
	DEBUG_COMM("start");
	if ((rv = get_bytes(reader_index, &c, 1)) != STATUS_SUCCESS)
		return rv;

	if (c == RDR_to_PC_NotifySlotChange)
		goto slot_change;

	if (c == SYNC)
		goto sync;

	if (c >= 0x80)
	{	
		DEBUG_COMM2("time request: 0x%02X", c);
		goto start;
	}

	DEBUG_CRITICAL2("Got 0x%02X", c);
	return STATUS_COMM_ERROR;

slot_change:
	DEBUG_COMM("slot change");
	if ((rv = get_bytes(reader_index, &c, 1)) != STATUS_SUCCESS)
		return rv;

	if (c == CARD_ABSENT)
	{
		DEBUG_COMM("Card removed");
	}
	else
		if (c == CARD_PRESENT)
		{
			DEBUG_COMM("Card inserted");
		}
		else
		{
			DEBUG_COMM2("Unknown card movement: %d", c);
		}
	goto start;

sync:
	DEBUG_COMM("sync");
	if ((rv = get_bytes(reader_index, &c, 1)) != STATUS_SUCCESS)
		return rv;

	if (c == CTRL_ACK)
		goto ack;

	if (c == CTRL_NAK)
		goto nak;

	DEBUG_CRITICAL2("Got 0x%02X instead of ACK/NAK", c);
	return STATUS_COMM_ERROR;

nak:
	DEBUG_COMM("nak");
	if ((rv = get_bytes(reader_index, &c, 1)) != STATUS_SUCCESS)
		return rv;

	if (c != (SYNC ^ CTRL_NAK))
	{
		DEBUG_CRITICAL2("Wrong LRC: 0x%02X", c);
		return STATUS_COMM_ERROR;
	}
	else
		goto start;

ack:
	DEBUG_COMM("ack");
	/* normal CCID frame */
	if ((rv = get_bytes(reader_index, buffer, 5)) != STATUS_SUCCESS)
		return rv;

	/* total frame size */
	to_read = 10+dw2i(buffer, 1);

	DEBUG_COMM2("frame size: %d", to_read);
	if ((rv = get_bytes(reader_index, buffer+5, to_read-5)) != STATUS_SUCCESS)
		return rv;

#ifdef DEBUG_LEVEL_COMM
		DEBUG_XXD("frame: ", buffer, to_read);
#endif

	/* lrc */
	DEBUG_COMM("lrc");
	if ((rv = get_bytes(reader_index, &c, 1)) != STATUS_SUCCESS)
		return rv;

	DEBUG_COMM2("lrc: 0x%02X", c);
	for (i=0; i<to_read; i++)
		c ^= buffer[i];

	if (c != (SYNC ^ CTRL_ACK))
		DEBUG_CRITICAL2("Wrong LRC: 0x%02X", c);

	if (echo)
	{
		echo = FALSE;
		goto start;
	}

	return STATUS_SUCCESS;
} /* ReadSerial */


/*****************************************************************************
 * 
 *				get_bytes: get n bytes
 *
 *****************************************************************************/
int get_bytes(unsigned int reader_index, unsigned char *buffer, int length)
{
	int offset = serialDevice[reader_index].buffer_offset;
	int offset_last = serialDevice[reader_index].buffer_offset_last;

	DEBUG_COMM3("available: %d, needed: %d", offset_last-offset,
		length);
	/* enough data are available */
	if (offset + length <= offset_last)
	{
		DEBUG_COMM("data available");
		memcpy(buffer, serialDevice[reader_index].buffer + offset, length);
		serialDevice[reader_index].buffer_offset += length;
	}
	else
	{
		int present, rv;

		/* copy available data */
		present = offset_last - offset;

		if (present > 0)
		{
			DEBUG_COMM2("some data available: %d", present);
			memcpy(buffer, serialDevice[reader_index].buffer + offset,
				present);
		}

		/* get fresh data */
		DEBUG_COMM2("get more data: %d", length - present);
		rv = ReadChunk(reader_index, serialDevice[reader_index].buffer,
			sizeof(serialDevice[reader_index].buffer), length - present);
		if (rv < 0)
			return STATUS_COMM_ERROR;

		/* fill the buffer */
		memcpy(buffer + present, serialDevice[reader_index].buffer,
			length - present);
		serialDevice[reader_index].buffer_offset = length - present;
		serialDevice[reader_index].buffer_offset_last = rv;
		DEBUG_COMM3("offset: %d, last_offset: %d",
			serialDevice[reader_index].buffer_offset,
			serialDevice[reader_index].buffer_offset_last);
	}

	return STATUS_SUCCESS;
} /* get_bytes */


/*****************************************************************************
 * 
 *				ReadChunk: read a minimum number of bytes
 *
 *****************************************************************************/
static int ReadChunk(unsigned int reader_index, unsigned char *buffer,
	int buffer_length, int min_length)
{
	int fd = serialDevice[reader_index].fd;
# ifndef S_SPLINT_S
	fd_set fdset;
# endif
	struct timeval t;
	int i, rv = 0;
	int already_read;
#ifdef DEBUG_LEVEL_COMM
	char debug_header[] = "<- 123456 ";

	sprintf(debug_header, "<- %06X ", reader_index);
#endif

	already_read = 0;
	while (already_read < min_length)
	{
		/* use select() to, eventually, timeout */
		FD_ZERO(&fdset);
		FD_SET(fd, &fdset);
		t.tv_sec = serialDevice[reader_index].ccid.readTimeout;
		t.tv_usec = 0;

		i = select(fd+1, &fdset, NULL, NULL, &t);
		if (i == -1)
		{
			DEBUG_CRITICAL2("select: %s", strerror(errno));
			return -1;
		}
		else
			if (i == 0)
			{
				DEBUG_COMM2("Timeout! (%d sec)", serialDevice[reader_index].ccid.readTimeout);
				return -1;
			}

		rv = read(fd, buffer + already_read, buffer_length - already_read);
		if (rv < 0)
		{
			DEBUG_COMM2("read error: %s", strerror(errno));
			return -1;
		}

#ifdef DEBUG_LEVEL_COMM
		DEBUG_XXD(debug_header, buffer + already_read, rv);
#endif

		already_read += rv;
		DEBUG_COMM3("read: %d, to read: %d", already_read,
			min_length);
	}

	return already_read;
} /* ReadChunk */


/*****************************************************************************
 * 
 *				OpenSerial: open the port
 *
 *****************************************************************************/
status_t OpenSerial(unsigned int reader_index, int channel)
{
	char dev_name[FILENAME_MAX];

	DEBUG_COMM3("Reader index: %X, Channel: %d", reader_index, channel);

	/*
	 * Conversion of old-style ifd-hanler 1.0 CHANNELID 
	 */
	if (channel == 0x0103F8)
		channel = 1;
	else
		if (channel == 0x0102F8)
			channel = 2;
		else
			if (channel == 0x0103E8)
				channel = 3;
			else
				if (channel == 0x0102E8)
					channel = 4;

	if (channel < 0)
	{
		DEBUG_CRITICAL2("wrong port number: %d", (int) channel);
		return STATUS_UNSUCCESSFUL;
	}

	sprintf(dev_name, "/dev/pcsc/%d", (int) channel);

	return OpenSerialByName(reader_index, dev_name);
} /* OpenSerial */

/*****************************************************************************
 * 
 *				OpenSerialByName: open the port
 *
 *****************************************************************************/
status_t OpenSerialByName(unsigned int reader_index, char *dev_name)
{
	struct termios current_termios;
	int i;
	unsigned int reader = reader_index;

	DEBUG_COMM3("Reader index: %X, Device: %s", reader_index, dev_name);

	/* check if the same channel is not already used */
	for (i=0; i<CCID_DRIVER_MAX_READERS; i++)
	{
		if (serialDevice[i].device &&
			strcmp(serialDevice[i].device, dev_name) == 0)
		{
			DEBUG_CRITICAL2("Device %s already in use", dev_name);
			return STATUS_UNSUCCESSFUL;
		}
	}

	serialDevice[reader].fd = open(dev_name, O_RDWR | O_NOCTTY);

	if (-1 == serialDevice[reader].fd)
	{
		DEBUG_CRITICAL3("open %s: %s", dev_name, strerror(errno));
		return STATUS_UNSUCCESSFUL;
	}

	/* set channel used */
	serialDevice[reader].device = strdup(dev_name);

	/* empty in and out serial buffers */
	if (tcflush(serialDevice[reader].fd, TCIOFLUSH))
			DEBUG_INFO2("tcflush() function error: %s", strerror(errno));

	/* get config attributes */
	if (tcgetattr(serialDevice[reader].fd, &current_termios) == -1)
	{
		DEBUG_INFO2("tcgetattr() function error: %s", strerror(errno));
		close(serialDevice[reader].fd);
		serialDevice[reader].fd = -1;

		return STATUS_UNSUCCESSFUL;
	}

	/* IGNBRK: ignore BREAK condition on input
	 * IGNPAR: ignore framing errors and parity errors. */
	current_termios.c_iflag = IGNBRK | IGNPAR;
	current_termios.c_oflag = 0;	/* Raw output modes */
	/* CS8: 8-bits character size
	 * CSTOPB: set two stop bits
	 * CREAD: enable receiver
	 * CLOCAL: ignore modem control lines */
	current_termios.c_cflag = CS8 | CSTOPB | CREAD | CLOCAL;

	/* Do not echo characters because if you connect to a host it or your modem
	 * will echo characters for you.  Don't generate signals. */
	current_termios.c_lflag = 0;

	/* set serial port speed to 115200 bauds */
	cfsetspeed(&current_termios, B115200);

	DEBUG_INFO("Set serial port baudrate to 115200 and correct configuration");
	if (tcsetattr(serialDevice[reader].fd, TCSANOW, &current_termios) == -1)
	{
		close(serialDevice[reader].fd);
		serialDevice[reader].fd = -1;
		DEBUG_INFO2("tcsetattr error: %s", strerror(errno));

		return STATUS_UNSUCCESSFUL;
	}

	serialDevice[reader].ccid.real_bSeq = 0;
	serialDevice[reader].ccid.pbSeq = &serialDevice[reader].ccid.real_bSeq;
	serialDevice[reader].ccid.readerID = GEMPCTWIN;
	serialDevice[reader].ccid.dwMaxCCIDMessageLength = 271;
	serialDevice[reader].ccid.dwMaxIFSD = 254;
	serialDevice[reader].ccid.dwFeatures = 0x00010230;
	serialDevice[reader].ccid.bPINSupport = 0x0;
	serialDevice[reader].ccid.dwDefaultClock = 4000;
	serialDevice[reader].ccid.dwMaxDataRate = 344086;
	serialDevice[reader].ccid.bMaxSlotIndex = 0;
	serialDevice[reader].ccid.bCurrentSlotIndex = 0;
	serialDevice[reader].ccid.arrayOfSupportedDataRates = SerialDataRates;

	serialDevice[reader].buffer_offset = 0;
	serialDevice[reader].buffer_offset_last = 0;

	/* perform a command to be sure a GemPC Twin reader is connected
	 * get the reader firmware */
	{
		unsigned char tx_buffer[] = { 0x02 };
		unsigned char rx_buffer[50];
		unsigned int rx_length = sizeof(rx_buffer);

		/* 2 seconds timeout to not wait too long if no reader is connected */
		serialDevice[reader].ccid.readTimeout = 2;

		if (IFD_SUCCESS != CmdEscape(reader_index, tx_buffer, sizeof(tx_buffer),
			rx_buffer, &rx_length))
		{
			DEBUG_CRITICAL("Get firmware failed. Maybe the reader is not connected");
			return STATUS_UNSUCCESSFUL;
		}

		/* normal timeout: 2 seconds */
		serialDevice[reader].ccid.readTimeout = DEFAULT_COM_READ_TIMEOUT ;

		rx_buffer[rx_length] = '\0';
		DEBUG_INFO2("Firmware: %s", rx_buffer);
	}

	/* perform a command to configure GemPC Twin reader card movement
	 * notification to synchronous mode: the card movement is notified _after_
	 * the host command and _before_ the reader anwser */
	{
		unsigned char tx_buffer[] = { 0x01, 0x01, 0x01};
		unsigned char rx_buffer[50];
		unsigned int rx_length = sizeof(rx_buffer);

		if (IFD_SUCCESS != CmdEscape(reader_index, tx_buffer, sizeof(tx_buffer),
			rx_buffer, &rx_length))
		{
			DEBUG_CRITICAL("Change card movement notification failed.");
			return STATUS_UNSUCCESSFUL;
		}
	}

	return STATUS_SUCCESS;
} /* OpenSerialByName */


/*****************************************************************************
 * 
 *				CloseSerial: close the port
 *
 *****************************************************************************/
status_t CloseSerial(unsigned int reader_index)
{
	unsigned int reader = reader_index;

	close(serialDevice[reader].fd);
	serialDevice[reader].fd = -1;

	free(serialDevice[reader].device);
	serialDevice[reader].device = NULL;

	return STATUS_SUCCESS;
} /* CloseSerial */


/*****************************************************************************
 *
 *					get_ccid_descriptor
 *
 ****************************************************************************/
_ccid_descriptor *get_ccid_descriptor(unsigned int reader_index)
{
	return &serialDevice[reader_index].ccid;
} /* get_ccid_descriptor */



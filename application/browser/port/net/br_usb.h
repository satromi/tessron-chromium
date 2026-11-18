/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_usb.h
 *	ブラウザ: the USB devices a program may use (the system's SVC class
 *	12, us_; port/net/br_usb.cc). Every call waits for the system's
 *	answer: the services' thread calls them on threads of their own.
 */

#ifndef BR_USB_H
#define BR_USB_H

#include <stdint.h>

#include <string>
#include <vector>

struct BrUsbDevice {
	int		dev;		/* the system's number of it */
	uint16_t	vendor, product, release;
	uint8_t		cls, subclass, protocol;
	uint32_t	hc, port;
	std::string	name;
};

/* The devices no class driver of the system took */
std::vector<BrUsbDevice> br_usb_list();

/* A descriptor of a device: its bytes, empty when it cannot be read */
std::vector<uint8_t> br_usb_descriptor( int dev, int type, int index, int len );

/* The system's errors the calls answer */
constexpr int kBrUsbStall = -41;	/* E_OBJ: the device stalled the endpoint */
constexpr int kBrUsbGone = -42;		/* E_NOEXS: the device went away, or no such endpoint */
constexpr int kBrUsbTimeout = -50;	/* E_TMOUT */
constexpr int kBrUsbBusy = -65;		/* E_BUSY: a class driver's or another program's */

/* Opened: the program's number for it, or an error (< 0) */
int br_usb_open( int dev );
void br_usb_close( int ud );

/* The transfers: the bytes moved, or an error (< 0); on an isochronous endpoint, one packet */
int br_usb_control( int ud, const uint8_t setup[8], uint8_t *data, int len, int ms );
int br_usb_transfer( int ud, int ep, uint8_t *data, int len, int ms );
bool br_usb_alternate( int ud, int ifno, int alt );
bool br_usb_clear_halt( int ud, int ep );
bool br_usb_reset( int ud );

#endif /* BR_USB_H */

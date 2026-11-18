/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_usb.cc
 *	ブラウザ: the USB devices a program may use, for WebUSB (design
 *	14.4.9): the system's calls of class 12 (us_, include/ts/usb.h).
 *	Listed are the devices no class driver took (role USB_ROLE_NONE, no
 *	hub); the system refuses to open any other.
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/usb.h>

#include "br_usb.h"

std::vector<BrUsbDevice> br_usb_list()
{
	static T_USBDEV		list[64];
	std::vector<BrUsbDevice> out;
	INT			n = us_lst_dev(list, 64);

	for ( INT i = 0; i < n && i < 64; i++ ) {
		const T_USBDEV &d = list[i];
		if ( d.role != USB_ROLE_NONE || d.hub_ports > 0 || d.dev_class == 0x09 ) continue;
		BrUsbDevice b;
		b.dev = d.dev;
		b.vendor = d.vendor;
		b.product = d.product;
		b.release = d.release;
		b.cls = d.dev_class;
		b.subclass = d.subclass;
		b.protocol = d.protocol;
		b.hc = d.hc;
		b.port = d.port;
		b.name = (const char *)d.name;
		out.push_back(b);
	}
	return out;
}

std::vector<uint8_t> br_usb_descriptor( int dev, int type, int index, int len )
{
	std::vector<uint8_t> b((size_t)len);
	INT n = us_get_dsc(dev, (UINT)type, (UINT)index, b.data(), len);
	b.resize(n > 0 ? (size_t)n : 0);
	return b;
}

int br_usb_open( int dev )
{
	return us_opn_dev(dev);
}

void br_usb_close( int ud )
{
	(void)us_cls_dev(ud);
}

int br_usb_control( int ud, const uint8_t setup[8], uint8_t *data, int len, int ms )
{
	return us_ctl(ud, setup, data, len, ms > 0 ? ms : TMO_FEVR);
}

int br_usb_transfer( int ud, int ep, uint8_t *data, int len, int ms )
{
	return us_xfer(ud, (UINT)ep, data, len, ms > 0 ? ms : TMO_FEVR);
}

bool br_usb_alternate( int ud, int ifno, int alt )
{
	return us_set_alt(ud, ifno, alt) >= E_OK;
}

bool br_usb_clear_halt( int ud, int ep )
{
	return us_clr_hlt(ud, (UINT)ep) >= E_OK;
}

bool br_usb_reset( int ud )
{
	return us_rst_dev(ud) >= E_OK;
}

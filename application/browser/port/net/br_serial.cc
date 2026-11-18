/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_serial.cc
 *	ブラウザ: the system's serial ports (include/ts/ser.h, the calls of
 *	SVC class 11) for Web Serial (design 14.4.9).
 */

#include TS_CONFIG_H
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ser.h>

#include <vector>

#include "br_serial.h"

std::vector<BrSerialPort> br_serial_list()
{
	std::vector<T_SRPORT>		list;
	std::vector<BrSerialPort>	out;
	INT				n = sr_lst_prt(nullptr, 0);	/* how many, then every one */

	if ( n <= 0 ) return out;
	list.resize((size_t)n);
	n = sr_lst_prt(list.data(), n);
	for ( INT i = 0; i < n && i < (INT)list.size(); i++ ) {
		BrSerialPort p;
		p.name = (const char *)list[i].name;
		p.label = (const char *)list[i].label;
		p.console = list[i].console != 0;
		p.speed = list[i].speed;
		out.push_back(p);
	}
	return out;
}

int br_serial_open( const std::string &name )
{
	return sr_opn_prt((CONST UB *)name.c_str());
}

void br_serial_close( int port )
{
	(void)sr_cls_prt(port);
}

int br_serial_read( int port, uint8_t *buf, int size, int ms )
{
	return sr_rea_prt(port, buf, size, ms);
}

int br_serial_write( int port, const uint8_t *buf, int size )
{
	return sr_wri_prt(port, buf, size);
}

bool br_serial_speed( int port, uint32_t speed )
{
	return sr_set_spd(port, speed) >= E_OK;
}

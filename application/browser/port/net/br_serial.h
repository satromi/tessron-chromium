/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_serial.h
 *	ブラウザ: the system's serial ports, for Web Serial (br_serial.cc).
 */

#ifndef BR_SERIAL_H
#define BR_SERIAL_H

#include <stdint.h>

#include <string>
#include <vector>

struct BrSerialPort {
	std::string	name;		/* the device: "sera", ... */
	std::string	label;		/* what the port is */
	bool		console;	/* the console's port */
	uint32_t	speed;		/* bits per second */
};

/* The machine's serial ports */
std::vector<BrSerialPort> br_serial_list();

/* A port opened by its device's name: its number (> 0), or <= 0 */
int br_serial_open( const std::string &name );
void br_serial_close( int port );

/* What has come in, up to size bytes, waiting up to ms for the first: the count, 0 for none, < 0 on an error */
int br_serial_read( int port, uint8_t *buf, int size, int ms );

/* Every byte sent: the count, < 0 on an error */
int br_serial_write( int port, const uint8_t *buf, int size );

/* The port's speed, bits per second */
bool br_serial_speed( int port, uint32_t speed );

#endif /* BR_SERIAL_H */

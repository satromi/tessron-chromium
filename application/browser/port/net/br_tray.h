/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	br_tray.h
 *	ブラウザ: the system's tray as the pages' clipboard (br_tray.cc).
 */

#ifndef BR_TRAY_H
#define BR_TRAY_H

#include <stdint.h>

#include <string>
#include <vector>

/*
 * A set put in the tray (it becomes the one in hand): the text as an
 * xmlTAD fragment of one paragraph a line, and a picture as PNG when
 * there is one; 'name' names the set. False when nothing went in.
 */
bool br_tray_put( const std::string &text, const std::vector<uint8_t> &png, const std::string &name );

/* The text of the set in hand: its first xmlTAD record, its tags taken out. False when there is none */
bool br_tray_text( std::string *text );

/* The first picture of the set in hand, PNG. False when there is none */
bool br_tray_png( std::vector<uint8_t> *png );

/* A number that changes whenever the set in hand does: 0 for an empty tray */
uint64_t br_tray_serial();

#endif /* BR_TRAY_H */

// Laying text out on the panel: cutting it with "…" and breaking it into lines. Widths come from Pen::measure, so an
// icon marker (icons.h) counts as its icon, and an arrow never starts a line. Tested in panel_test.cpp.
#pragma once

#include "draw.h"

#include <cstddef>
#include <string>
#include <vector>

/**
 * Length in bytes of the UTF-8 character that starts with this byte.
 * @param lead the first byte
 * @return 1..4
 */
size_t utf8Length(unsigned char lead);

/**
 * Shorten text with "…" until it fits (an icon goes as a whole).
 * @param pen drawing tools
 * @param text the text
 * @param size text size
 * @param bold whether bold
 * @param maxWidth the width to fit
 * @param keepEnd keep the end and cut the start (for file paths)
 * @return the text that fits
 */
std::string ellipsize(const Pen& pen, const std::string& text, double size, bool bold, double maxWidth, bool keepEnd);

/**
 * Break text into lines that fit a width. Breaks at spaces, and between any two CJK characters, but never before
 * a closing mark (the character before it moves down with it) or an arrow icon (it stays at the end of the line
 * before, with the word in front of it).
 * The last allowed line is shortened with "…" if the text does not fit.
 * @param pen drawing tools
 * @param text the text
 * @param size text size
 * @param bold whether bold
 * @param maxWidth line width
 * @param maxLines most lines
 * @return the lines
 */
std::vector<std::string> wrapText(const Pen& pen, const std::string& text, double size, bool bold, double maxWidth,
                                  size_t maxLines);

/**
 * The largest text size (down to a minimum) at which wrapped text fits in so many lines without being cut.
 * @param pen drawing tools
 * @param text the text
 * @param size the size to start from
 * @param minSize never smaller than this (the text may be cut there)
 * @param bold whether bold
 * @param maxWidth line width
 * @param maxLines most lines
 * @return the size
 */
double wrapSize(const Pen& pen, const std::string& text, double size, double minSize, bool bold, double maxWidth,
                size_t maxLines);

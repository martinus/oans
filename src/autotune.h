/*
 * autotune.h
 *
 * Empirical --io-threads calibration: measure hashing throughput on the real
 * target at several thread counts and recommend (and optionally persist) the
 * fastest. This is the authoritative counterpart to the storage heuristic in
 * storage.c, which only guesses from device type.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef	__AUTOTUNE_H__
#define	__AUTOTUNE_H__

/*
 * Run the calibration over `roots` and print a recommendation. If
 * options.hashfile is set, the winning io-threads is stored in that hashfile's
 * config so later runs pick it up automatically. Returns a process exit code.
 */
int autotune_run(char **roots, int nroots);

#endif	/* __AUTOTUNE_H__ */

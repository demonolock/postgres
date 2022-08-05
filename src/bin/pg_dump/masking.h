/*-------------------------------------------------------------------------
 *
 * masking.h
 *
 *	Data masking tool for pg_dump
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/masking.h
 *
 *-------------------------------------------------------------------------
 */
#include <sys/file.h>
#include <stdio.h>

typedef struct
{
	FILE	   *fp;
	const char *filename;
	int			lineno;
	int		is_error;
} MaskingStateData;


int
parse_masking_params(const char *filename, void *dopt);
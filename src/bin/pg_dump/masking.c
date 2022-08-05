/*-------------------------------------------------------------------------
 *
 * Data masking tool for pg_dump
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pg_dump/masking.c
 *
 *-------------------------------------------------------------------------
 */

#include "masking.h"
#include <stdio.h>


int
parse_masking_params(const char *filename, void *dopt)
{
    MaskingStateData *masking_data;
    masking_data->fp = fopen(filename, "r");
    if (!masking_data->fp)
    {
        printf("could not open filter file \"%s\"\n", filename);
        return 1;
    }

	masking_data->is_error = 1;

	
    return 0;
}
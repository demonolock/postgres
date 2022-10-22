/*-------------------------------------------------------------------------
 *
 * masking.h
 *
 *	Masking functionality for pg_dump
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/masking.h
 *
 *-------------------------------------------------------------------------
 */

#include "dumputils.h"

#ifndef MASKING_H
#define MASKING_H

typedef struct
{
	char* column; 	/* name of masked column */
	char* table;	/* name of table where masked column is stored */
	char* func;		/* name of masking function */
	char* schema;	/* name of schema where masking function is stored */
} MaskColumnInfo;


/*
* mask_column_info_list contains info about every to-be-masked column:
* its name, a name of its table (if nothing is specified - mask all columns with this name),
* name of masking function and name of schema containing this function (public if not specified)
*/

static SimplePtrList mask_column_info_list = {NULL, NULL};
SimpleStringList mask_columns_list = {NULL, NULL};
SimpleStringList mask_func_list = {NULL, NULL};

static void formMaskingLists(DumpOptions* dopt);
static void addFuncToDatabase(MaskColumnInfo* cur_mask_column_info, 
							 FILE* mask_func_file, PGconn *connection);
static void maskColumns(TableInfo *tbinfo, char* current_column_name,
						PQExpBuffer* q, SimpleStringList* column_names);


#endif							/* MASKING_H */

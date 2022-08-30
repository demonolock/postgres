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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>



typedef struct _pair {
    char *key;
    char *value;
} Pair;

typedef struct _maskingMap {
    Pair **data;
    int size;
    int capacity;
} MaskingMap;

enum
ParsingState
{
    SCHEMA_NAME,
    TABLE_NAME,
    FIELD_NAME,
    FUNCTION_NAME,
    WAIT_COLON,
    WAIT_OPEN_BRACE,
    WAIT_CLOSE_BRACE,
    WAIT_COMMA
};

struct
MaskingDebugDetails
{
    int line_num;
    int symbol_num;
    enum ParsingState parsing_state;
};

MaskingMap *newMaskingMap();
void printMap(MaskingMap *map);
int readMaskingPatternFromFile(FILE * fin, MaskingMap *map);
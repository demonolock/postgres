/*-------------------------------------------------------------------------
 *
 * masking.c
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include "masking.h"

char *DEFAULT_NAME="default";
const char REL_SEP = '.'; /* Relation separator */
const size_t REL_SIZE = 64 * 8; /* Length of relation name - 64 bytes */
const int COL_WITH_FUNC_SIZE = 3 * REL_SIZE + 3; // schema_name.function name + '(' + column_name + ')

int exit_status;
int brace_counter;
int close_brace_counter;
bool skip_reading;
char *col_with_func;
char c;

MaskingMap *
newMaskingMap() {
    MaskingMap *map = malloc(sizeof(MaskingMap));
    map->size = 0;
    map->capacity = 8;
    map->data = malloc(sizeof(Pair) * map->capacity);
    memset(map->data, 0, sizeof(Pair) * map->capacity);
    return map;
}

int
getMapIndexByKey(MaskingMap *map, char *key) {
    int index = 0;
    while (map->data[index] != NULL) {
        if (strcmp(map->data[index]->key, key) == 0) {
            return index;
        }
        index++;
    }
    return -1;
}

void
cleanMap(MaskingMap *map) {
    if (map != NULL && map->data != NULL) {
        for (int i = 0; map->data[i] != NULL; i++) {
            printf("key: %s, value: %s\n", (char *) map->data[i]->key, (char *) map->data[i]->value);
            free(map->data[i]->key);
            free(map->data[i]->value);
            free(map->data[i]);

        }
        printf("capacity:%d, size:%d\n", map->capacity, map->size);
        free(map->data);
        free(map);
    }
}

void
setMapValue(MaskingMap *map, char *key, char *value) {
    int index = getMapIndexByKey(map, key);
    if (index != -1) // Already have key in map
    {
        free(map->data[index]->value);
        map->data[index]->value = malloc(strlen(value) + 1);
        strcpy(map->data[index]->value, value);

    } else {

        Pair *pair = malloc(sizeof(Pair));
        pair->key = malloc(strlen(key) + 1);
        pair->value = malloc(strlen(value) + 1);
        memset(pair->key, 0, strlen(key));
        memset(pair->value, 0, strlen(value));
        strcpy(pair->key, key);
        strcpy(pair->value, value);

        map->data[map->size] = malloc(sizeof(Pair));
        *map->data[map->size] = *pair;
        map->size++;
        free(pair);
    }
    if (map->size == map->capacity) { /* Increase capacity */
        map->capacity *= 1.5;
        map->data = realloc(map->data, sizeof(Pair) * map->capacity);
    }
    free(key);
}

void
printParsingError(struct MaskingDebugDetails *md, char *message, char current_symbol) {
    printf("Error position (symbol '%c'): line: %d pos: %d. %s\n", current_symbol, md->line_num, md->symbol_num,
           message);
};

bool
isTerminal(char c) {
    return c == ':' || c == ',' || c == '{' || c == '}' || c == EOF;
};

bool
isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == EOF;
};

char
readNextSymbol(struct MaskingDebugDetails *md, FILE *fin) {
    char c = fgetc(fin);
    if (c == '\n') {
        md->line_num++;
        md->symbol_num = 1;
    } else {
        md->symbol_num++;
    }
    return tolower(c);
};

/* Read relation name*/
char
nameReader(char *rel_name, char c, struct MaskingDebugDetails *md, FILE *fin) {
    memset(rel_name, 0, REL_SIZE);
    while (!isTerminal(c)) {

        switch (c) {
            case ' ':
            case '\t':
            case '\n':
                break; /* Skip space symbols */
            case EOF:
                return c;

            default:
                strncat(rel_name, &c, 1);
                break;
        }
        c = readNextSymbol(md, fin);
    }
    return c;
};

char *
getFullRelName(char *schema_name, char *table_name, char *field_name) {
    char *full_name = malloc(REL_SIZE * 3); /* Schema.Table.Field */
    memset(full_name, 0, REL_SIZE * 3);
    strcpy(full_name, schema_name);
    strncat(full_name, &REL_SEP, 1);
    strcat(full_name, table_name);
    strncat(full_name, &REL_SEP, 1);
    strcat(full_name, field_name);
    return full_name;
}

int
readMaskingPatternFromFile(FILE *fin, MaskingMap *map) {
    struct MaskingDebugDetails md;
    md.line_num = 1;
    md.symbol_num = 0;
    md.parsing_state = SCHEMA_NAME;
    exit_status = EXIT_SUCCESS;

    md.schema_name = malloc(REL_SIZE);
    md.table_name = malloc(REL_SIZE);
    md.field_name = malloc(REL_SIZE);
    md.func_name = malloc(REL_SIZE);

    brace_counter = 0;
    close_brace_counter = 0;
    skip_reading = false;

    c = ' ';
    while (c != EOF) {
        if (skip_reading) {
            skip_reading = false;
        } else if (!isTerminal(c)) {
            c = readNextSymbol(&md, fin);
        }
        switch (md.parsing_state) {
            case SCHEMA_NAME:
                c = nameReader(md.schema_name, c, &md, fin);
                md.parsing_state = WAIT_OPEN_BRACE;
                memset(md.table_name, 0, sizeof REL_SIZE);
                break;

            case TABLE_NAME:
                c = nameReader(md.table_name, c, &md, fin);
                md.parsing_state = WAIT_OPEN_BRACE;
                break;

            case FIELD_NAME:
                c = nameReader(md.field_name, c, &md, fin);
                md.parsing_state = WAIT_COLON;
                break;

            case FUNCTION_NAME:
                c = nameReader(md.func_name, c, &md, fin);
                setMapValue(map, getFullRelName(md.schema_name, md.table_name, md.field_name), md.func_name);
                md.parsing_state = WAIT_COMMA;
                break;

            case WAIT_COLON:
                if (isSpace(c))
                    break;
                if (c != ':') {
                    printParsingError(&md, "Waiting symbol ':'", c);
                    exit_status = EXIT_FAILURE;
                    goto clear_resources;
                }
                md.parsing_state = FUNCTION_NAME;
                c = readNextSymbol(&md, fin);
                skip_reading = true;
                break;

            case WAIT_OPEN_BRACE:
                if (isSpace(c))
                    break;
                if (c == '}' && brace_counter > 0) {
                    md.parsing_state = WAIT_CLOSE_BRACE;
                    break;
                }
                if (c != '{') {
                    printParsingError(&md, "Waiting symbol '{'", c);
                    exit_status = EXIT_FAILURE;
                    goto clear_resources;
                }
                if (md.table_name[0] != '\0') /* we have already read table_name */
                {
                    md.parsing_state = FIELD_NAME;
                } else {
                    md.parsing_state = TABLE_NAME;
                }
                c = readNextSymbol(&md, fin);
                skip_reading = true;
                brace_counter++;
                break;

            case WAIT_CLOSE_BRACE:
                if (isSpace(c))
                    break;
                if (c != '}') {
                    printParsingError(&md, "Waiting symbol '}'", c);
                    exit_status = EXIT_FAILURE;
                    goto clear_resources;
                }
                md.parsing_state = TABLE_NAME;
                c = readNextSymbol(&md, fin);
                brace_counter--;
                break;

            case WAIT_COMMA:
                if (isSpace(c))
                    break;
                if (c == '}') {
                    c = readNextSymbol(&md, fin);
                    skip_reading = true;
                    close_brace_counter++;
                    break;
                }
                if (c != ',' && !isTerminal(c)) /* Schema_name or Table_name */
                {
                    if (close_brace_counter == 1) {
                        md.parsing_state = TABLE_NAME;
                    } else if (close_brace_counter == 2) {
                        md.parsing_state = SCHEMA_NAME;
                    } else {
                        printParsingError(&md, "Too many symbols '}'", c);
                        exit_status = EXIT_FAILURE;
                        goto clear_resources;
                    }
                    skip_reading = true;
                    close_brace_counter = 0;
                    break;
                } else if (c != ',') {
                    printParsingError(&md, "Waiting symbol ','", c);
                    exit_status = EXIT_FAILURE;
                    goto clear_resources;
                }
                md.parsing_state = FIELD_NAME;
                c = readNextSymbol(&md, fin);
                skip_reading = true;
                break;
        }
    }
    clear_resources:
    free(md.schema_name);
    free(md.table_name);
    free(md.field_name);
    free(md.func_name);
    return exit_status;
}

// Creating string in format `schema_name.function name(column_name)`
void
concatFunctionAndColumn(char *col_with_func, char *schema_name, char *column, char *function_name, MaskingMap *map)
{
    strcpy(col_with_func, schema_name);
    strcat(col_with_func, ".");
    strcat(col_with_func, function_name);
    strcat(col_with_func, "(");
    strcat(col_with_func, column);
    strcat(col_with_func, ")");
}


char *
addFunctionToColumn(char *schema_name, char *table_name, char *column_name, MaskingMap *map)
{
    int index=getMapIndexByKey(map, getFullRelName(schema_name, table_name, column_name));
    if (index == -1)
    {
        // For all schemas [default.table.field]
        index = getMapIndexByKey(map, getFullRelName(DEFAULT_NAME, table_name, column_name));
        if (index == -1)
        {
            // For all tables and schemas [default.default.field]
            index = getMapIndexByKey(map, getFullRelName(DEFAULT_NAME, DEFAULT_NAME, column_name));
            if (index == -1)
            {
                // For all fields in all schemas and tables [default.default.default]
                index = getMapIndexByKey(map, getFullRelName(DEFAULT_NAME, DEFAULT_NAME, DEFAULT_NAME));
            }
        }
    }
    col_with_func = malloc(COL_WITH_FUNC_SIZE + 1);
    memset(col_with_func, 0, COL_WITH_FUNC_SIZE + 1);
    if (index != -1)
    {
        char *function_name=map->data[index]->value;
        if (strcmp(function_name, DEFAULT_NAME)!=0)
        {
            concatFunctionAndColumn(col_with_func, schema_name, column_name,  function_name, map);
        }
        else
        {
            // TODO [USE default function]
            concatFunctionAndColumn(col_with_func, schema_name, column_name,  "default_function_1", map);
        }
    }

    return col_with_func;
}

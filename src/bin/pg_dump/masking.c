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
#include "masking.h"

char REL_SEP = '.'; /* Relation separator */
size_t size = 64 * 8; /* Length of relation name - 64 bytes */

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
            //printf("key: %s, value: %s\n", (char *) map->data[i]->key, (char *) map->data[i]->value);
            free(map->data[i]->key);
            free(map->data[i]->value);
            free(map->data[i]);

        }
        //printf("capacity:%d, size:%d\n", map->capacity, map->size);
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
    return c;
};

/* Read relation name*/
char
nameReader(char *rel_name, char c, struct MaskingDebugDetails *md, FILE *fin) {
    memset(rel_name, 0, size);
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
    char *full_name = malloc(size * 3); /* Schema.Table.Field */
    memset(full_name, 0, size * 3);
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
    int exit_status = EXIT_SUCCESS;

    char *schema_name = malloc(size);
    char *table_name = malloc(size);
    char *field_name = malloc(size);
    char *func_name = malloc(size);

    int brace_counter = 0;
    int close_brace_counter = 0;
    bool skip_reading = false;

    char c = ' ';
    while (c != EOF) {
        if (skip_reading) {
            skip_reading = false;
        } else if (!isTerminal(c)) {
            c = readNextSymbol(&md, fin);
        }
        switch (md.parsing_state) {
            case SCHEMA_NAME:
                c = nameReader(schema_name, c, &md, fin);
                md.parsing_state = WAIT_OPEN_BRACE;
                memset(table_name, 0, sizeof size);
                break;

            case TABLE_NAME:
                c = nameReader(table_name, c, &md, fin);
                md.parsing_state = WAIT_OPEN_BRACE;
                break;

            case FIELD_NAME:
                c = nameReader(field_name, c, &md, fin);
                md.parsing_state = WAIT_COLON;
                break;

            case FUNCTION_NAME:
                c = nameReader(func_name, c, &md, fin);
                setMapValue(map, getFullRelName(schema_name, table_name, field_name), func_name);
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
                if (table_name[0] != '\0') /* we have already read table_name */
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
    free(schema_name);
    free(table_name);
    free(field_name);
    free(func_name);
    return exit_status;
}

char *
addFunctionToColumn(char *schema_name, char *table_name, char *column, MaskingMap *map) {
    int index=getMapIndexByKey(map, getFullRelName(schema_name, table_name, column));
    int col_with_func_size = 3 * size + 3; // schema_name.function name + '(' + column_name + ')
    char *col_with_func= malloc(col_with_func_size + 1);
    memset(col_with_func, 0, col_with_func_size + 1);
    if (index != -1)
    {
        strcpy(col_with_func, schema_name);
        strcat(col_with_func, ".");
        strcat(col_with_func, map->data[index]->value);
        strcat(col_with_func, "(");
        strcat(col_with_func, column);
        strcat(col_with_func, ")");
    }
    return col_with_func;
}

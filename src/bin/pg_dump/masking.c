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

#define REL_SIZE 64 * 8 /* Length of relation name - 64 bytes */
#define DEFAULT_NAME "default"
#define COL_WITH_FUNC_SIZE 3 * REL_SIZE + 3 /* schema_name.function name + '(' + column_name + ') */
#define PATH_SIZE 255 * 8 + 2 /* length of the path + 2 quotes */

char REL_SEP = '.'; /* Relation separator */

MaskingMap *
newMaskingMap()
{
    MaskingMap *map = malloc(sizeof(MaskingMap));
    map->size = 0;
    map->capacity = 8;
    map->data = malloc(sizeof(Pair) * map->capacity);
    memset(map->data, 0, sizeof(Pair) * map->capacity);
    return map;
}

int
getMapIndexByKey(MaskingMap *map, char *key)
{
    int index = 0;
    while (map->data[index] != NULL)
    {
        if (strcmp(map->data[index]->key, key) == 0)
        {
            return index;
        }
        index++;
    }
    return -1;
}

void
cleanMap(MaskingMap *map)
{
    if (map != NULL && map->data != NULL)
    {
        for (int i = 0; map->data[i] != NULL; i++)
        {
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
setMapValue(MaskingMap *map, char *key, char *value)
{
    if (key != NULL)
    {
        int index = getMapIndexByKey(map, key);
        if (index != -1) /* Already have key in map */
        {
            free(map->data[index]->value);
            map->data[index]->value = malloc(strlen(value) + 1);
            strcpy(map->data[index]->value, value);
        }
        else
        {
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
        if (map->size == map->capacity) /* Increase capacity */
        {
            map->capacity *= 1.5;
            map->data = realloc(map->data, sizeof(Pair) * map->capacity);
        }
    }
    free(key);
}

void
printParsingError(struct MaskingDebugDetails *md, char *message, char current_symbol)
{
    printf("Error position (symbol '%c'): line: %d pos: %d. %s\n", current_symbol, md->line_num, md->symbol_num,
           message);
}

bool
isTerminal(char c)
{
    return c == ':' || c == ',' || c == '{' || c == '}' || c == EOF;
}

bool
isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == EOF;
}

char
readNextSymbol(struct MaskingDebugDetails *md, FILE *fin)
{
    char c = fgetc(fin);
    if (c == '\n')
    {
        md->line_num++;
        md->symbol_num = 1;
    }
    else
    {
        md->symbol_num++;
    }
    return tolower(c);
}

/* Read relation name*/
char
nameReader(char *rel_name, char c, struct MaskingDebugDetails *md, FILE *fin)
{
    memset(rel_name, 0, PATH_SIZE);
    while (!isTerminal(c))
    {
        switch (c)
        {
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
}

char *
getFullRelName(char *schema_name, char *table_name, char *field_name)
{
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
readMaskingPatternFromFile(FILE *fin, MaskingMap *map, SimpleStringList *func_query_path)
{
    int exit_status;
    int brace_counter;
    int close_brace_counter;
    char *schema_name;
    char *table_name;
    char *field_name;
    char *func_name;
    bool skip_reading;
    char c;

    struct MaskingDebugDetails md;
    md.line_num = 1;
    md.symbol_num = 0;
    md.parsing_state = SCHEMA_NAME;
    exit_status = EXIT_SUCCESS;

    schema_name = malloc(REL_SIZE + 1);
    table_name = malloc(REL_SIZE + 1);
    field_name = malloc(REL_SIZE + 1);
    func_name = malloc(PATH_SIZE + 1); /* We can get function name or path to file with a creating function query */

    brace_counter = 0;
    close_brace_counter = 0;
    skip_reading = false;

    c = ' ';
    while (c != EOF)
    {
        if (skip_reading)
        {
            skip_reading = false;
        }
        else if (!isTerminal(c))
        {
            c = readNextSymbol(&md, fin);
        }
        switch (md.parsing_state)
        {
            case SCHEMA_NAME:
                c = nameReader(schema_name, c, &md, fin);
                md.parsing_state = WAIT_OPEN_BRACE;
                memset(table_name, 0, sizeof REL_SIZE);
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
                extractFuncNameIfPath(func_name, func_query_path);
                setMapValue(map, getFullRelName(schema_name, table_name, field_name), func_name);
                md.parsing_state = WAIT_COMMA;
                break;

            case WAIT_COLON:
                if (isSpace(c))
                    break;
                if (c != ':')
                {
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
                if (c == '}' && brace_counter > 0)
                {
                    md.parsing_state = WAIT_CLOSE_BRACE;
                    break;
                }
                if (c != '{')
                {
                    printParsingError(&md, "Waiting symbol '{'", c);
                    exit_status = EXIT_FAILURE;
                    goto clear_resources;
                }
                if (table_name[0] != '\0') /* we have already read table_name */
                {
                    md.parsing_state = FIELD_NAME;
                }
                else
                {
                    md.parsing_state = TABLE_NAME;
                }
                c = readNextSymbol(&md, fin);
                skip_reading = true;
                brace_counter++;
                break;

            case WAIT_CLOSE_BRACE:
                if (isSpace(c))
                    break;
                if (c != '}')
                {
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
                if (c == '}')
                {
                    c = readNextSymbol(&md, fin);
                    skip_reading = true;
                    close_brace_counter++;
                    break;
                }
                if (c != ',' && !isTerminal(c)) /* Schema_name or Table_name */
                {
                    if (close_brace_counter == 1)
                    {
                        md.parsing_state = TABLE_NAME;
                    }
                    else if (close_brace_counter == 2)
                    {
                        md.parsing_state = SCHEMA_NAME;
                    }
                    else
                    {
                        printParsingError(&md, "Too many symbols '}'", c);
                        exit_status = EXIT_FAILURE;
                        goto clear_resources;
                    }
                    skip_reading = true;
                    close_brace_counter = 0;
                    break;
                }
                else if (c != ',')
                {
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

/* Creating string in format `schema_name.function name(column_name)` */
void
concatFunctionAndColumn(char *col_with_func, char *schema_name, char *column_name, char *function_name)
{
    strcpy(col_with_func, schema_name);
    strcat(col_with_func, ".");
    strcat(col_with_func, function_name);
    strcat(col_with_func, "(");
    strcat(col_with_func, column_name);
    strcat(col_with_func, ")");
}


char *
addFunctionToColumn(char *schema_name, char *table_name, char *column_name, MaskingMap *map)
{
    char *col_with_func;
    int index = getMapIndexByKey(map, getFullRelName(schema_name, table_name, column_name));
    if (index == -1)
    {
        /* For all schemas [default.table.field] */
        index = getMapIndexByKey(map, getFullRelName(DEFAULT_NAME, table_name, column_name));
        if (index == -1)
        {
            /* For all tables and schemas [default.default.field] */
            index = getMapIndexByKey(map, getFullRelName(DEFAULT_NAME, DEFAULT_NAME, column_name));
            if (index == -1)
            {
                /* For all fields in all schemas and tables [default.default.default] */
                index = getMapIndexByKey(map, getFullRelName(DEFAULT_NAME, DEFAULT_NAME, DEFAULT_NAME));
            }
        }
    }
    col_with_func = malloc(COL_WITH_FUNC_SIZE + 1);
    memset(col_with_func, 0, COL_WITH_FUNC_SIZE + 1);
    if (index != -1)
    {
        char *function_name = map->data[index]->value;
        if (strcmp(function_name, DEFAULT_NAME) != 0)
        {
            /* We can get an error if function is not available */
            concatFunctionAndColumn(col_with_func, schema_name, column_name, function_name);
        }
        else
        {
            concatFunctionAndColumn(col_with_func, schema_name, column_name, "default");
        }
    }
    return col_with_func;
}

void
removeQuotes(char *func_name)
{
    char *new_func_name = malloc(PATH_SIZE + 1);
    strncpy(new_func_name, func_name + 1, strlen(func_name) - 2);
    memset(func_name, 0, PATH_SIZE);
    strcpy(func_name, new_func_name);
}


/**
 * Read a word from a query, that creating a custom function
 */
char *
readWord(FILE *fin, char *word)
{
    char c;
    memset(word, 0, strlen(word));
    do
    {
        c = tolower(getc(fin));
        if (isSpace(c) || c == '(') /* Space or open brace before function arguments */
        {
            if (word[0] == '\0') /* Spaces before the word */
                continue;
            else
                break; /* Spaces after the word */
        }
        else
        {
            strncat(word, &c, 1);
        }
    } while (c != EOF);
    return word;
}

int
extractFunctionNameFromQueryFile(char *filename, char *func_name)
{
    FILE *fin;
    char *word;

    memset(func_name, 0, REL_SIZE);
    fin = NULL;
    if (filename[0] != '\0')
    {
        fin = fopen(filename, "r");
    }
    if (fin == NULL)
    {
        printf("Problem with file \'%s\"", filename);
    }
    else
    {
        word = malloc(REL_SIZE + 1);
        memset(word, 0, REL_SIZE);
        if (strcmp(readWord(fin, word), "create") == 0)
        {
            if (strcmp(readWord(fin, word), "or") == 0) /* read 'or' | 'function' */
            {
                if (strcmp(readWord(fin, word), "replace") != 0)
                {
                    printf("Keyword 'replace' was expected, but found '%s'. Check query for creating a function '%s'.\n",
                           word, filename);
                    goto free_resources;
                }
                else
                {
                    readWord(fin, word); /* read 'function' */
                }
            }
        }
        else
        {
            printf("Keyword 'create' was expected, but found '%s'. Check query for creating a function '%s'.\n", word,
                   filename);
            goto free_resources;
        }
        if (strcmp(word, "function"))
        {
            strcpy(func_name, readWord(fin, word));
        }
        else
        {
            printf("Keyword 'function' was expected, but found '%s'. Check query for creating a function '%s'.\n", word,
                   filename);
            goto free_resources;
        }
        free_resources:
        free(word);
        fclose(fin);
    }
    return func_name[0] != '\0'; /* If we got a function name, then - return 0, else - return 1 */
}

/**
 * If there is a path (the first symbol is a quote '"'), then store this path in maskingMap->funcQueryPath
 * and write to the first argument (func_path) name of the function from the query in the file
 * If there is not a path - do nothing
*/
void
extractFuncNameIfPath(char *func_path, SimpleStringList *func_query_path)
{
    char *func_name;
    if (func_path[0] == '"')
    {
        func_name = malloc(REL_SIZE + 1);
        removeQuotes(func_path);
        if (extractFunctionNameFromQueryFile(func_path, func_name) ==
            0) /* Read function name from query and store in func_name */
        {
            if (!simple_string_list_member(func_query_path, func_path))
            {
                simple_string_list_append(func_query_path, func_path);
            }
            strcpy(func_path, func_name); /* Store func_name in func_path to throw it to upper function */
        }
        free(func_name);
    }
}

char *
readQueryForCreatingFunction(char *filename)
{
    FILE *fin;
    char *query;
    long fsize;

    query = malloc(sizeof(char));
    memset(query, 0, sizeof(char));
    fin = fopen(filename, "r");
    if (fin == NULL)
    {
        fseek(fin, 0L, SEEK_END);
        fsize = ftell(fin);
        fseek(fin, 0L, SEEK_SET);

        query = (char *) calloc(fsize, sizeof(char));

        fread(query, sizeof(char), fsize, fin);

        fclose(fin);
    }
    return query;
}
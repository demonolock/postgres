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

#include "masking.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

int
readMaskingPatternFromFile(FILE *fin, MaskingRulesTree *rules_tree)
{
    struct MaskingDebugDetails md;
    md.line_num = 1;
    md.symbol_num = 0;
    md.parsing_state = TABLE_NAME;

    char *root_name = "Root";
    char table_name[64] = "";
    char field_name[64] = "";
    char function_name[64] = ""; /* Max length of a table or a field name */

    char c = ' ';
    while(c != EOF)
    {
        if (!isTerminal(c))
        {
            c = readNextSymbol(&md, fin);
        }

        switch(md.parsing_state)
        {
            case TABLE_NAME:
                memset(table_name, 0, sizeof table_name);
                c = nameReader(table_name, c, &md, fin);
                addNode(rules_tree, root_name, table_name);
                md.parsing_state = WAIT_OPEN_BRACE;
                break;

            case FIELD_NAME:
                memset(field_name, 0, sizeof field_name);
                c = nameReader(field_name, c, &md, fin);
                addNode(rules_tree, table_name, field_name);
                md.parsing_state = WAIT_COLON;
                break;

            case FUNCTION_NAME:
                memset(function_name, 0, sizeof function_name);
                c = nameReader(function_name, c, &md, fin);
                addNode(rules_tree, field_name, function_name);
                md.parsing_state = WAIT_COMMA;
                break;

            case WAIT_COLON:
                if (isSpace(c))
                    break;
                if (c != ':')
                {
                    printParsingError(&md, "Waiting symbol ':'", c);
                    return EXIT_FAILURE;
                }
                md.parsing_state = FUNCTION_NAME;
                c = readNextSymbol(&md, fin);
                break;

            case WAIT_OPEN_BRACE:
                if (isSpace(c))
                    break;
                if (c != '{')
                {
                    printParsingError(&md, "Waiting symbol '{'", c);
                    return EXIT_FAILURE;
                }
                md.parsing_state = FIELD_NAME;
                c = readNextSymbol(&md, fin);
                break;

            case WAIT_CLOSE_BRACE:
                if (isSpace(c))
                    break;
                if (c != '{')
                {
                    printParsingError(&md, "Waiting symbol '}'", c);
                    return EXIT_FAILURE;
                }
                md.parsing_state = TABLE_NAME;
                c = readNextSymbol(&md, fin);
                break;

            case WAIT_COMMA:
                if (isSpace(c))
                    break;
                if (c == '}')
                {
                    md.parsing_state = TABLE_NAME;
                    c = readNextSymbol(&md, fin);
                    break;
                }
                if (c != ',')
                {
                    printParsingError(&md, "Waiting symbol ','", c);
                    return EXIT_FAILURE;
                }
                md.parsing_state = FIELD_NAME;
                c = readNextSymbol(&md, fin);
                break;

        }
    }
    return EXIT_SUCCESS;
}

/*
 * Functions for file parsing
 */
void
printParsingError(struct MaskingDebugDetails *md, char *message, char current_symbol)
{
    printf("Error position (symbol '%c'): line: %d pos: %d. %s\n", current_symbol, md->line_num, md->symbol_num, message);
}

bool
isTerminal(char c)
{
    return c == ':' || c == ',' || c == '{' || c == '}' || c == EOF;
};

bool
isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\000';
}

char
readNextSymbol(struct MaskingDebugDetails *md, FILE *fin)
{
    char c = fgetc(fin);
    if (c == '\n')
    {
        md->line_num++;
        md->symbol_num=1;
    }
    else {
        md->symbol_num++;
    }
    return c;
}

/* Read name of table, function or table field */
char
nameReader(char *name, char c, struct MaskingDebugDetails *md, FILE *fin)
{
    while(!isTerminal(c)) {

        switch(c)
        {
            case ' ':
            case '\000':
            case '\t':
            case '\n':
                break; /* Skip space symbols */

            default:
                strncat(name, &c, 1);
                break;
        }
        c = readNextSymbol(md, fin);
    }

    return c;
};

/*
 * Functions for work with n-tree
 *                            Root
 *                              |
 *             |--------------------------------|
 *           Table1            ...            TableN
 *       |-------------|                  |-------------|
 *    Field1   ...  FieldN              Field1   ...  FieldN
 *       |             |                  |             |
 *   Function1 ... FunctionN           Function1 ... FunctionN
 *
 */
MaskingRulesTree *
reserveMemoryForNode(const char *node_name)
{
    MaskingRulesTree *node = malloc(sizeof(MaskingRulesTree));

    if (node != NULL)
    {
        node->child = NULL;
        node->name = malloc(sizeof(node_name));
        strcpy(node->name, node_name);
        node->next = NULL;
    }

    return node;
}

MaskingRulesTree *
addSibling(const char *node_name, MaskingRulesTree *node)
{
    MaskingRulesTree *new_node = reserveMemoryForNode(node_name);

    if (node == NULL)
    {
        node = new_node;
    }
    else
    {
        MaskingRulesTree *aux = node;

        while (aux->next != NULL)
        {
            aux = aux->next;
        }

        aux->next = new_node;
    }

    return node;
}

MaskingRulesTree *
addNode(MaskingRulesTree *node, const char *name_root, const char *node_name)
{
    if (node == NULL || node_name == NULL)
    {
        return NULL;
    }

    if (strcmp(node->name, name_root) == 0) /* Are equal */
    {
        node->child = addSibling(node_name, node->child);

        return node;
    }

    MaskingRulesTree *found;
    /* Search in Siblings */
    if ((found = addNode(node->next, name_root, node_name)) != NULL)
    {
        return found;
    }

    if ((found = addNode(node->child, name_root, node_name)) != NULL)
    {
        return found;
    }

    return NULL;
}

void
printTabs(int level)
{
    for (int i = 0; i < level; i++)
    {
        putchar('\t');
    }
}

int
printTreeRecursive(MaskingRulesTree *node, int level)
{
    while (node != NULL)
    {
        printTabs(level);
        printf("%s\n", node->name);

        if (node->child != NULL)
        {
            printTabs(level);
            printTreeRecursive(node->child, level + 1);
        }

        node = node->next;
    }
    return EXIT_SUCCESS;
}

void
printTree(MaskingRulesTree *node)
{
    printTreeRecursive(node, 0);
}
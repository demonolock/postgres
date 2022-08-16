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
#include <stdbool.h>


enum /* States during file parsing */
ParsingState
{
    TABLE_NAME,
    FIELD_NAME,
    FUNCTION_NAME,
    WAIT_COLON,
    WAIT_OPEN_BRACE,
    WAIT_CLOSE_BRACE,
    WAIT_COMMA
};

struct /* Marks the line with parsing error */
MaskingDebugDetails
{
    int line_num;
    int symbol_num;
    enum ParsingState parsing_state;
};

/**
* N-tree definition
*/
typedef struct _Tree
{
    struct _Tree *child;
    struct _Tree *next;
    char *name;
} MaskingRulesTree;

void
printParsingError(struct MaskingDebugDetails *md, char *message, char current_symbol);

bool
isTerminal(char c);

bool
isSpace(char c);

char
readNextSymbol(struct MaskingDebugDetails *md, FILE *fin);

char
nameReader(char * name, char c, struct MaskingDebugDetails * md, FILE * fin);

MaskingRulesTree *
reserveMemoryForNode(const char *node_name);

MaskingRulesTree *
addSibling(const char *node_name, MaskingRulesTree *node);

MaskingRulesTree *
addNode(MaskingRulesTree *node, const char *name_root, const char *node_name);

void
printTabs(int level);

int
printTreeRecursive(MaskingRulesTree *node, int level);

void
printTree(MaskingRulesTree *node);

int
readMaskingPatternFromFile(FILE * fin, MaskingRulesTree *rules_tree);
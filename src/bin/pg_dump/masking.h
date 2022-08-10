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
print_parsing_error(struct MaskingDebugDetails *md, char *message, char current_symbol);

bool
is_terminal(char c);

bool
is_space(char c);

char
read_next_symbol(struct MaskingDebugDetails *md, FILE *fin);

char
name_reader(char * name, char c, struct MaskingDebugDetails * md, FILE * fin);

MaskingRulesTree *
reserve_memory_for_node(const char *node_name);

MaskingRulesTree *
add_sibling(const char *node_name, MaskingRulesTree *node);

MaskingRulesTree *
add_node(MaskingRulesTree *node, const char *name_root, const char *node_name);

void
print_tabs(int level);

void
print_tree_recursive(MaskingRulesTree *node, int level);

void
print_tree(MaskingRulesTree *node);

int
read_masking_pattern_from_file(FILE * fin, MaskingRulesTree *rules_tree);
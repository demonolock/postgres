/*-------------------------------------------------------------------------
 *
 * masking.h
 *
 *	  Data masking for pg_dump: header with types and prototypes
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/masking.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MASKING_H
#define MASKING_H

#include "libpq-fe.h"
#include "pqexpbuffer.h"

/*
 * Built-in masking strategies.
 *
 * Each strategy maps to a pure inline SQL expression — nothing is created
 * on the server.
 */
typedef enum MaskingStrategy
{
	MASK_CONSTANT,		/* replace with a fixed literal */
	MASK_NULL,			/* replace with NULL */
	MASK_ZERO,			/* type-appropriate zero value */
	MASK_SCRAMBLE,		/* md5 hash, preserves approximate length */
	MASK_PARTIAL,		/* keep last N chars, mask the rest */
	MASK_FAKE_EMAIL,	/* deterministic fake email */
	MASK_FAKE_NAME,		/* deterministic fake name */
	MASK_RANDOM_INT,	/* deterministic int in [min, max] */
	MASK_RANDOM_DATE,	/* deterministic date in a range */
	MASK_NOISE,			/* percentage noise on numeric values */
	MASK_DEFAULT,		/* type-appropriate full replacement */
	MASK_SQL,			/* user-supplied SQL expression */
	MASK_FUNCTION		/* call an existing server-side function */
} MaskingStrategy;

/*
 * One masking rule, parsed from a single line of the config file.
 *
 * schema_pattern / table_pattern / column_pattern may be "*" (wildcard)
 * or "default" (fallback), or an exact name.
 *
 * type_pattern is non-NULL only for @type rules.
 */
typedef struct MaskingRule
{
	char		   *schema_pattern;
	char		   *table_pattern;
	char		   *column_pattern;
	char		   *type_pattern;		/* NULL unless @type rule */
	MaskingStrategy	strategy;
	char		   *str_param;			/* value=, expr=, name= */
	int				int_param;			/* last=, min=, variance= */
	int				int_param2;			/* max for range */
} MaskingRule;

/*
 * Complete masking configuration, suitable for embedding in DumpOptions.
 */
typedef struct MaskingConfig
{
	MaskingRule	   *rules;
	int				nrules;
	int				capacity;
	char		   *salt;		/* optional salt for deterministic masking */
} MaskingConfig;

/* ---- public API ---- */

extern MaskingConfig *parseMaskingConfig(const char *filename);
extern void freeMaskingConfig(MaskingConfig *conf);

extern char *getMaskingExpression(const MaskingConfig *conf,
								 const char *schema_name,
								 const char *table_name,
								 const char *column_name,
								 const char *type_name);

#endif							/* MASKING_H */

/*-------------------------------------------------------------------------
 *
 * masking.c
 *
 *	  Data masking for pg_dump.
 *
 *	  Parses a line-oriented config file and generates inline SQL expressions
 *	  for each masked column.  Zero server-side modifications — all masking is
 *	  expressed via pure SQL in the SELECT list of COPY (SELECT ...) TO or
 *	  DECLARE cursor FOR SELECT queries.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/bin/pg_dump/masking.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <limits.h>

#include "common/logging.h"
#include "dumputils.h"
#include "fe_utils/string_utils.h"
#include "masking.h"

/* Initial capacity for the rules array */
#define MASKING_INIT_CAPACITY	32

/* Maximum length of a single config line */
#define MASKING_LINE_MAXLEN		2048

/* -------- forward declarations -------- */
static bool parseMaskingLine(const char *line, int lineno,
							 MaskingConfig *conf);
static MaskingStrategy parseStrategy(const char *name);
static bool parseKeyValue(const char *token, const char *key, char **value);
static void addRule(MaskingConfig *conf, MaskingRule *rule);
static int	matchScore(const MaskingRule *rule,
					   const char *schema_name,
					   const char *table_name,
					   const char *column_name,
					   const char *type_name);
static bool patternMatch(const char *pattern, const char *name);
static char *generateExpression(const MaskingRule *rule,
								const char *column_name,
								const char *type_name,
								const char *salt);
static char *defaultExprForType(const char *type_name, const char *salt,
								const char *column_name);
static const char *baseTypeName(const char *type_name);

/* ================================================================
 *                       CONFIG PARSER
 * ================================================================
 *
 * Config format (one rule per line, # comments):
 *
 *   schema.table.column    strategy    [key=value ...]
 *   *.*.column             strategy
 *   default.default.email  fake_email
 *   @timestamptz           constant    value=2000-01-01T00:00:00Z
 *   .salt = my_secret
 */

/*
 * parseMaskingConfig
 *
 *		Parse the masking config file.  Returns a MaskingConfig on success,
 *		or NULL on failure (errors are logged).
 */
MaskingConfig *
parseMaskingConfig(const char *filename)
{
	FILE	   *fp;
	char		line[MASKING_LINE_MAXLEN];
	int			lineno = 0;
	MaskingConfig *conf;

	if (filename == NULL || filename[0] == '\0')
	{
		pg_log_error("--masking requires a non-empty filename");
		return NULL;
	}

	fp = fopen(filename, "r");
	if (fp == NULL)
	{
		pg_log_error("could not open masking file \"%s\": %m", filename);
		return NULL;
	}

	conf = pg_malloc0(sizeof(MaskingConfig));
	conf->capacity = MASKING_INIT_CAPACITY;
	conf->rules = pg_malloc0(sizeof(MaskingRule) * conf->capacity);
	conf->nrules = 0;
	conf->salt = pg_strdup("");

	while (fgets(line, sizeof(line), fp) != NULL)
	{
		lineno++;

		if (!parseMaskingLine(line, lineno, conf))
		{
			fclose(fp);
			freeMaskingConfig(conf);
			return NULL;
		}
	}

	fclose(fp);

	if (conf->nrules == 0)
	{
		pg_log_error("masking file \"%s\" contains no rules", filename);
		freeMaskingConfig(conf);
		return NULL;
	}

	return conf;
}

/*
 * parseMaskingLine
 *
 *		Parse one line of the config file.  Returns true on success.
 */
static bool
parseMaskingLine(const char *line, int lineno, MaskingConfig *conf)
{
	char		buf[MASKING_LINE_MAXLEN];
	char	   *p;
	char	   *target;
	char	   *strategy_str;
	char	   *token;
	char	   *dot1;
	char	   *dot2;
	MaskingRule	rule;

	/* copy so we can mutate */
	strlcpy(buf, line, sizeof(buf));

	/* strip trailing newline */
	p = buf + strlen(buf) - 1;
	while (p >= buf && (*p == '\n' || *p == '\r'))
		*p-- = '\0';

	/* skip leading whitespace */
	p = buf;
	while (*p && isspace((unsigned char) *p))
		p++;

	/* skip empty lines and comments */
	if (*p == '\0' || *p == '#')
		return true;

	/* --- handle .salt directive --- */
	if (*p == '.')
	{
		/* .salt = value */
		p++;
		while (*p && isspace((unsigned char) *p))
			p++;
		if (strncmp(p, "salt", 4) == 0)
		{
			p += 4;
			while (*p && isspace((unsigned char) *p))
				p++;
			if (*p == '=')
			{
				p++;
				while (*p && isspace((unsigned char) *p))
					p++;
				/* trim trailing whitespace */
				{
					char *end = p + strlen(p) - 1;
					while (end > p && isspace((unsigned char) *end))
						*end-- = '\0';
				}
				if (conf->salt)
					pg_free(conf->salt);
				conf->salt = pg_strdup(p);
				return true;
			}
		}
		pg_log_error("masking config line %d: invalid directive", lineno);
		return false;
	}

	/* --- parse target (first token) --- */
	target = p;
	while (*p && !isspace((unsigned char) *p))
		p++;
	if (*p)
		*p++ = '\0';

	/* skip whitespace before strategy */
	while (*p && isspace((unsigned char) *p))
		p++;
	strategy_str = p;
	while (*p && !isspace((unsigned char) *p))
		p++;
	if (*p)
		*p++ = '\0';

	if (strategy_str[0] == '\0')
	{
		pg_log_error("masking config line %d: missing strategy", lineno);
		return false;
	}

	memset(&rule, 0, sizeof(rule));

	/* --- parse target: @type or schema.table.column --- */
	if (target[0] == '@')
	{
		/* type-based rule: @typename */
		rule.schema_pattern = pg_strdup("*");
		rule.table_pattern = pg_strdup("*");
		rule.column_pattern = pg_strdup("*");
		rule.type_pattern = pg_strdup(target + 1);
	}
	else
	{
		/* schema.table.column */
		dot1 = strchr(target, '.');
		if (dot1 == NULL)
		{
			pg_log_error("masking config line %d: target must be schema.table.column or @type",
						 lineno);
			return false;
		}
		*dot1 = '\0';
		dot2 = strchr(dot1 + 1, '.');
		if (dot2 == NULL)
		{
			pg_log_error("masking config line %d: target must be schema.table.column (need two dots)",
						 lineno);
			return false;
		}
		*dot2 = '\0';

		rule.schema_pattern = pg_strdup(target);
		rule.table_pattern = pg_strdup(dot1 + 1);
		rule.column_pattern = pg_strdup(dot2 + 1);
		rule.type_pattern = NULL;
	}

	/* --- parse strategy --- */
	rule.strategy = parseStrategy(strategy_str);
	if ((int) rule.strategy == -1)
	{
		pg_log_error("masking config line %d: unknown strategy \"%s\"",
					 lineno, strategy_str);
		return false;
	}

	/* --- parse optional key=value parameters --- */
	rule.str_param = NULL;
	rule.int_param = 0;
	rule.int_param2 = 0;

	while (*p)
	{
		while (*p && isspace((unsigned char) *p))
			p++;
		if (*p == '\0' || *p == '#')
			break;

		token = p;
		while (*p && !isspace((unsigned char) *p))
			p++;
		if (*p)
			*p++ = '\0';

		if (parseKeyValue(token, "value", &rule.str_param))
			continue;
		if (parseKeyValue(token, "expr", &rule.str_param))
			continue;
		if (parseKeyValue(token, "name", &rule.str_param))
			continue;

		{
			char *val = NULL;

			if (parseKeyValue(token, "last", &val))
			{
				rule.int_param = atoi(val);
				pg_free(val);
				continue;
			}
			if (parseKeyValue(token, "min", &val))
			{
				rule.int_param = atoi(val);
				pg_free(val);
				continue;
			}
			if (parseKeyValue(token, "max", &val))
			{
				rule.int_param2 = atoi(val);
				pg_free(val);
				continue;
			}
			if (parseKeyValue(token, "variance", &val))
			{
				rule.int_param = atoi(val);
				pg_free(val);
				continue;
			}
			if (parseKeyValue(token, "days", &val))
			{
				rule.int_param = atoi(val);
				pg_free(val);
				continue;
			}
		}

		pg_log_error("masking config line %d: unknown parameter \"%s\"",
					 lineno, token);
		return false;
	}

	addRule(conf, &rule);
	return true;
}

/*
 * parseStrategy — map a strategy name string to its enum value.
 * Returns -1 on unknown name.
 */
static MaskingStrategy
parseStrategy(const char *name)
{
	if (strcmp(name, "constant") == 0)
		return MASK_CONSTANT;
	if (strcmp(name, "null") == 0)
		return MASK_NULL;
	if (strcmp(name, "zero") == 0)
		return MASK_ZERO;
	if (strcmp(name, "scramble") == 0)
		return MASK_SCRAMBLE;
	if (strcmp(name, "partial") == 0)
		return MASK_PARTIAL;
	if (strcmp(name, "fake_email") == 0)
		return MASK_FAKE_EMAIL;
	if (strcmp(name, "fake_name") == 0)
		return MASK_FAKE_NAME;
	if (strcmp(name, "random_int") == 0)
		return MASK_RANDOM_INT;
	if (strcmp(name, "random_date") == 0)
		return MASK_RANDOM_DATE;
	if (strcmp(name, "noise") == 0)
		return MASK_NOISE;
	if (strcmp(name, "default") == 0)
		return MASK_DEFAULT;
	if (strcmp(name, "sql") == 0)
		return MASK_SQL;
	if (strcmp(name, "function") == 0)
		return MASK_FUNCTION;

	return (MaskingStrategy) -1;
}

/*
 * parseKeyValue — check if token is "key=value".  If so, set *value to a
 * pg_strdup'd copy of the value part and return true.
 */
static bool
parseKeyValue(const char *token, const char *key, char **value)
{
	size_t	keylen = strlen(key);

	if (strncmp(token, key, keylen) == 0 && token[keylen] == '=')
	{
		if (*value)
			pg_free(*value);
		*value = pg_strdup(token + keylen + 1);
		return true;
	}
	return false;
}

/*
 * addRule — append a rule to the config, growing the array if needed.
 */
static void
addRule(MaskingConfig *conf, MaskingRule *rule)
{
	if (conf->nrules >= conf->capacity)
	{
		conf->capacity *= 2;
		conf->rules = pg_realloc(conf->rules,
								 sizeof(MaskingRule) * conf->capacity);
	}
	conf->rules[conf->nrules] = *rule;	/* struct copy */
	conf->nrules++;
}

/*
 * freeMaskingConfig — release all memory.
 */
void
freeMaskingConfig(MaskingConfig *conf)
{
	int		i;

	if (conf == NULL)
		return;

	for (i = 0; i < conf->nrules; i++)
	{
		MaskingRule *r = &conf->rules[i];

		if (r->schema_pattern)
			pg_free(r->schema_pattern);
		if (r->table_pattern)
			pg_free(r->table_pattern);
		if (r->column_pattern)
			pg_free(r->column_pattern);
		if (r->type_pattern)
			pg_free(r->type_pattern);
		if (r->str_param)
			pg_free(r->str_param);
	}
	if (conf->rules)
		pg_free(conf->rules);
	if (conf->salt)
		pg_free(conf->salt);
	pg_free(conf);
}


/* ================================================================
 *                       RULE MATCHING
 * ================================================================ */

/*
 * patternMatch — does pattern match name?
 *
 *   "*"       matches anything
 *   "default" matches anything (fallback)
 *   otherwise exact, case-sensitive match
 */
static bool
patternMatch(const char *pattern, const char *name)
{
	if (strcmp(pattern, "*") == 0)
		return true;
	if (strcmp(pattern, "default") == 0)
		return true;
	return (strcmp(pattern, name) == 0);
}

/*
 * matchScore — compute a specificity score for a rule against a given column.
 *
 * Returns -1 if the rule does not match at all.
 * Higher score = more specific match.
 *
 * Scoring:
 *   exact schema  +40
 *   exact table   +20
 *   exact column  +10
 *   "default" for schema/table/column adds 0 (it matches but isn't specific)
 *   "*" for schema/table/column adds 0
 *   @type match adds +5
 */
static int
matchScore(const MaskingRule *rule,
		   const char *schema_name,
		   const char *table_name,
		   const char *column_name,
		   const char *type_name)
{
	int		score = 0;

	/* type-based rule */
	if (rule->type_pattern != NULL)
	{
		const char *base = baseTypeName(type_name);

		if (strcmp(rule->type_pattern, type_name) != 0 &&
			strcmp(rule->type_pattern, base) != 0)
			return -1;			/* type doesn't match */

		/* for @type rules, schema/table/column are wildcards */
		return 5;
	}

	/* schema */
	if (!patternMatch(rule->schema_pattern, schema_name))
		return -1;
	if (strcmp(rule->schema_pattern, schema_name) == 0)
		score += 40;

	/* table */
	if (!patternMatch(rule->table_pattern, table_name))
		return -1;
	if (strcmp(rule->table_pattern, table_name) == 0)
		score += 20;

	/* column */
	if (!patternMatch(rule->column_pattern, column_name))
		return -1;
	if (strcmp(rule->column_pattern, column_name) == 0)
		score += 10;

	return score;
}

/*
 * getMaskingExpression
 *
 *		Find the best-matching rule for (schema, table, column, type) and
 *		generate an inline SQL expression.
 *
 *		Returns a pg_malloc'd string, or NULL if no rule matches (column
 *		should be dumped as-is).
 */
char *
getMaskingExpression(const MaskingConfig *conf,
					const char *schema_name,
					const char *table_name,
					const char *column_name,
					const char *type_name)
{
	int		best_score = -1;
	int		best_idx = -1;
	int		i;

	for (i = 0; i < conf->nrules; i++)
	{
		int		s = matchScore(&conf->rules[i],
							   schema_name, table_name,
							   column_name, type_name);
		if (s > best_score)
		{
			best_score = s;
			best_idx = i;
		}
	}

	if (best_idx < 0)
		return NULL;

	return generateExpression(&conf->rules[best_idx],
							  column_name, type_name,
							  conf->salt);
}


/* ================================================================
 *                   SQL EXPRESSION GENERATION
 * ================================================================
 *
 * Every strategy generates a pure SQL expression that can appear in a
 * SELECT list.  No server-side objects are created.
 */

/*
 * baseTypeName — strip "character varying", "timestamp with time zone" etc.
 * to a short canonical name for type-based dispatch.
 */
static const char *
baseTypeName(const char *type_name)
{
	if (type_name == NULL)
		return "text";
	if (strncmp(type_name, "character varying", 17) == 0)
		return "varchar";
	if (strncmp(type_name, "character", 9) == 0)
		return "char";
	if (strncmp(type_name, "timestamp with time zone", 24) == 0)
		return "timestamptz";
	if (strncmp(type_name, "timestamp without time zone", 27) == 0)
		return "timestamp";
	if (strncmp(type_name, "timestamp", 9) == 0)
		return "timestamp";
	if (strncmp(type_name, "time with time zone", 19) == 0)
		return "timetz";
	if (strncmp(type_name, "time without time zone", 22) == 0)
		return "time";
	if (strncmp(type_name, "double precision", 16) == 0)
		return "float8";
	if (strcmp(type_name, "real") == 0)
		return "float4";
	if (strcmp(type_name, "smallint") == 0)
		return "int2";
	if (strcmp(type_name, "bigint") == 0)
		return "int8";
	if (strncmp(type_name, "numeric", 7) == 0)
		return "numeric";
	if (strcmp(type_name, "boolean") == 0)
		return "bool";
	return type_name;
}

/*
 * isNumericType — is this base type name numeric?
 */
static bool
isNumericType(const char *base)
{
	return (strcmp(base, "int2") == 0 ||
			strcmp(base, "int4") == 0 ||
			strcmp(base, "integer") == 0 ||
			strcmp(base, "int8") == 0 ||
			strcmp(base, "float4") == 0 ||
			strcmp(base, "float8") == 0 ||
			strcmp(base, "numeric") == 0 ||
			strcmp(base, "money") == 0);
}

/*
 * isDateTimeType — is this a date/time type?
 */
static bool
isDateTimeType(const char *base)
{
	return (strcmp(base, "date") == 0 ||
			strcmp(base, "timestamp") == 0 ||
			strcmp(base, "timestamptz") == 0 ||
			strcmp(base, "time") == 0 ||
			strcmp(base, "timetz") == 0 ||
			strcmp(base, "interval") == 0);
}

/*
 * defaultExprForType — generate a "default" masking expression appropriate
 * to the column type.  Deterministic: same input -> same output.
 */
static char *
defaultExprForType(const char *type_name, const char *salt,
				   const char *column_name)
{
	const char *base = baseTypeName(type_name);
	PQExpBuffer buf = createPQExpBuffer();
	char	   *result;

	if (isNumericType(base))
	{
		appendPQExpBuffer(buf, "0::%s", type_name);
	}
	else if (strcmp(base, "bool") == 0)
	{
		appendPQExpBufferStr(buf, "true");
	}
	else if (strcmp(base, "date") == 0)
	{
		appendPQExpBufferStr(buf, "'1900-01-01'::date");
	}
	else if (strcmp(base, "timestamp") == 0)
	{
		appendPQExpBufferStr(buf, "'1900-01-01 00:00:00'::timestamp");
	}
	else if (strcmp(base, "timestamptz") == 0)
	{
		appendPQExpBufferStr(buf, "'1900-01-01 00:00:00+00'::timestamptz");
	}
	else if (strcmp(base, "time") == 0)
	{
		appendPQExpBufferStr(buf, "'00:00:00'::time");
	}
	else if (strcmp(base, "timetz") == 0)
	{
		appendPQExpBufferStr(buf, "'00:00:00+00'::timetz");
	}
	else if (strcmp(base, "interval") == 0)
	{
		appendPQExpBufferStr(buf, "'0'::interval");
	}
	else if (strcmp(base, "uuid") == 0)
	{
		appendPQExpBufferStr(buf,
			"'00000000-0000-0000-0000-000000000000'::uuid");
	}
	else if (strcmp(base, "json") == 0)
	{
		appendPQExpBufferStr(buf, "'{}'::json");
	}
	else if (strcmp(base, "jsonb") == 0)
	{
		appendPQExpBufferStr(buf, "'{}'::jsonb");
	}
	else if (strcmp(base, "bytea") == 0)
	{
		appendPQExpBufferStr(buf, "'\\x00'::bytea");
	}
	else if (strcmp(base, "inet") == 0)
	{
		appendPQExpBufferStr(buf, "'0.0.0.0'::inet");
	}
	else if (strcmp(base, "cidr") == 0)
	{
		appendPQExpBufferStr(buf, "'0.0.0.0/0'::cidr");
	}
	else if (strcmp(base, "macaddr") == 0)
	{
		appendPQExpBufferStr(buf, "'00:00:00:00:00:00'::macaddr");
	}
	else
	{
		/* text / varchar / char / anything else — replace with 'XXXX' */
		appendPQExpBuffer(buf, "'XXXX'::%s", type_name);
	}

	result = pg_strdup(buf->data);
	destroyPQExpBuffer(buf);
	return result;
}

/*
 * generateExpression — build the SQL expression for one rule + column.
 *
 * The returned string is pg_malloc'd.
 */
static char *
generateExpression(const MaskingRule *rule,
				   const char *column_name,
				   const char *type_name,
				   const char *salt)
{
	PQExpBuffer buf = createPQExpBuffer();
	char	   *result;
	const char *col = fmtId(column_name);
	const char *safe_salt = salt ? salt : "";

	switch (rule->strategy)
	{
		case MASK_CONSTANT:
			{
				const char *val = rule->str_param ? rule->str_param : "XXXX";

				appendPQExpBuffer(buf, "'%s'::%s", val, type_name);
			}
			break;

		case MASK_NULL:
			appendPQExpBuffer(buf, "NULL::%s", type_name);
			break;

		case MASK_ZERO:
			{
				const char *base = baseTypeName(type_name);

				if (isNumericType(base))
					appendPQExpBuffer(buf, "0::%s", type_name);
				else if (isDateTimeType(base))
				{
					if (strcmp(base, "date") == 0)
						appendPQExpBufferStr(buf, "'1970-01-01'::date");
					else if (strcmp(base, "timestamp") == 0)
						appendPQExpBufferStr(buf, "'1970-01-01 00:00:00'::timestamp");
					else if (strcmp(base, "timestamptz") == 0)
						appendPQExpBufferStr(buf, "'1970-01-01 00:00:00+00'::timestamptz");
					else
						appendPQExpBuffer(buf, "'00:00:00'::%s", type_name);
				}
				else
					appendPQExpBuffer(buf, "''::%s", type_name);
			}
			break;

		case MASK_SCRAMBLE:
			appendPQExpBuffer(buf,
				"md5('%s' || %s::text)::varchar(32)",
				safe_salt, col);
			break;

		case MASK_PARTIAL:
			{
				int last = rule->int_param > 0 ? rule->int_param : 4;

				appendPQExpBuffer(buf,
					"overlay(%s::text placing repeat('X', greatest(length(%s::text) - %d, 0)) from 1 for greatest(length(%s::text) - %d, 0))",
					col, col, last, col, last);
			}
			break;

		case MASK_FAKE_EMAIL:
			appendPQExpBuffer(buf,
				"'user_' || substr(md5('%s' || %s::text), 1, 8) || '@masked.invalid'",
				safe_salt, col);
			break;

		case MASK_FAKE_NAME:
			appendPQExpBuffer(buf,
				"'name_' || substr(md5('%s' || %s::text), 1, 8)",
				safe_salt, col);
			break;

		case MASK_RANDOM_INT:
			{
				int	min = rule->int_param;
				int	max = rule->int_param2 > min ? rule->int_param2 : min + 1000;

				appendPQExpBuffer(buf,
					"(abs(hashtext('%s' || %s::text)) %% %d + %d)",
					safe_salt, col, max - min, min);
			}
			break;

		case MASK_RANDOM_DATE:
			{
				int	days = rule->int_param > 0 ? rule->int_param : 3650;

				appendPQExpBuffer(buf,
					"'2000-01-01'::date + (abs(hashtext('%s' || %s::text)) %% %d)",
					safe_salt, col, days);
			}
			break;

		case MASK_NOISE:
			{
				int	variance = rule->int_param > 0 ? rule->int_param : 10;

				appendPQExpBuffer(buf,
					"%s * (1.0 + (hashtext('%s' || %s::text) %% %d) / 100.0)",
					col, safe_salt, col, variance);
			}
			break;

		case MASK_DEFAULT:
			result = defaultExprForType(type_name, salt, column_name);
			destroyPQExpBuffer(buf);
			return result;

		case MASK_SQL:
			if (rule->str_param)
				appendPQExpBufferStr(buf, rule->str_param);
			else
				appendPQExpBufferStr(buf, col);
			break;

		case MASK_FUNCTION:
			if (rule->str_param)
				appendPQExpBuffer(buf, "%s(%s)", rule->str_param, col);
			else
				appendPQExpBufferStr(buf, col);
			break;
	}

	result = pg_strdup(buf->data);
	destroyPQExpBuffer(buf);
	return result;
}

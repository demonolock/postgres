# pg_dump --masking: Design Document

## Problem Statement

DBAs need to create sanitized copies of production databases for dev/staging/QA
environments while complying with GDPR, HIPAA, PCI-DSS. The current `--masking`
implementation has critical issues that prevent production use:

1. **Modifies production database** — executes `CREATE FUNCTION` / `CREATE SCHEMA`
   on the live cluster, which is unacceptable for read-only replicas and
   change-managed production environments.
2. **Breaks constraints** — `XXXX` for all strings violates UNIQUE, CHECK, FK.
3. **SQL injection** — identifiers are not escaped with `fmtId()`.
4. **Custom DSL** — 300+ line hand-written parser for a bespoke config format.
5. **No pattern matching** — must enumerate every column explicitly.
6. **Memory safety** — buffer overflows, leaks, `char` instead of `int` for `fgetc`.

## Prior Art

### PostgreSQL Anonymizer (Dalibo)

- Rules stored as `SECURITY LABEL FOR anon ON COLUMN ...`
- Rich function library: `fake_*`, `pseudo_*`, `partial()`, `noise()`, `hash()`
- Deterministic masking via `pseudo_*` (seed + salt) preserves FK integrity
- `pg_dump_anon` tool for anonymous dumps
- Server-side extension — requires installation on the cluster

### Other Solutions

- **Oracle Data Masking**: format-preserving encryption, deterministic masking
- **SQL Server Dynamic Data Masking**: `partial()`, `email()`, `random()`
- **MySQL**: no built-in dump-time masking; third-party tools like `mysql-anon`

### Key Insight

All mature solutions provide: (a) pattern-based rules, (b) deterministic masking
for FK consistency, (c) built-in strategies beyond simple replacement. None of
them require creating server-side objects at dump time.

## Design Principles

1. **Zero server-side modifications** — all masking via inline SQL expressions
   in `COPY (SELECT ...) TO` and `DECLARE cursor FOR SELECT ...`
2. **Simple, familiar config** — one line per rule, similar to `pg_hba.conf`
3. **Pattern matching** — wildcards for schema/table/column names
4. **Built-in strategies** — expressed as pure SQL using PostgreSQL built-in
   functions (`md5`, `hashtext`, `overlay`, etc.)
5. **Deterministic by default** — same input always produces same output,
   preserving JOINs across tables
6. **Proper pg_dump integration** — config in `DumpOptions`, no globals

## Config Format

One line = one rule. Fields separated by whitespace.

```
# schema.table.column    strategy    [key=value ...]

# Exact match
public.users.email            fake_email
public.users.phone            partial     last=4
public.users.ssn              constant    value=XXX-XX-XXXX

# Wildcards: * matches any component
public.*.email                fake_email
*.*.phone                     partial     last=4
public.users.*                constant    value=REDACTED

# "default" = fallback for unmatched schemas/tables
default.default.email         fake_email

# Type-based rules (prefix @)
@timestamptz                  constant    value=2000-01-01T00:00:00Z

# Inline SQL expression
public.users.birthdate        sql         expr=date_trunc('year',birthdate)

# Existing server function (must already exist in the cluster)
public.users.name             function    name=my_schema.mask_name

# Global salt for deterministic masking (optional, default: empty)
# .salt = my_secret_salt_value
```

### Why This Format

- No nested braces — no custom parser needed, `strtok`/`sscanf` suffices
- Each line is self-contained — easy to generate, validate, merge, diff
- Familiar to DBAs — similar style to `.pgpass`, `pg_hba.conf`
- Supports `#` comments
- ~80 lines of parser code vs 300+ in the current implementation

## Built-in Masking Strategies

Each strategy generates an inline SQL expression. No server-side functions needed.

| Strategy      | Generated SQL Expression                                          | Description                           |
|---------------|-------------------------------------------------------------------|---------------------------------------|
| `constant`    | `'value'::type`                                                   | Fixed replacement value               |
| `null`        | `NULL::type`                                                      | Replace with NULL                     |
| `zero`        | `0` / `''` / `'1970-01-01'` (by type)                            | Zero value for the type               |
| `scramble`    | `md5('salt' \|\| col::text)::varchar(N)`                         | Hash, preserves approximate length    |
| `partial`     | `overlay(col placing repeat('X',...) from 1 for ...)`            | Keep last N chars visible             |
| `fake_email`  | `'user_' \|\| md5('salt' \|\| col::text)::varchar(8) \|\| '@masked.invalid'` | Unique valid email     |
| `fake_name`   | `'name_' \|\| md5('salt' \|\| col::text)::varchar(8)`           | Unique pseudonym                      |
| `random_int`  | `(hashtext(col::text) % (max-min)) + min`                        | Deterministic int in range            |
| `random_date` | `'2000-01-01'::date + (hashtext(col::text) % N)`                | Deterministic date in range           |
| `noise`       | `col * (1.0 + (hashtext(col::text) % var) / 100.0)`             | Add percentage noise to numbers       |
| `default`     | Chosen by column type (zero for numbers, 'XXXX' for text, etc.)  | Full replacement                      |
| `sql`         | User-provided expression verbatim                                 | Arbitrary SQL                         |
| `function`    | `schema.func(col)`                                                | Call existing server function          |

### Determinism

All strategies using `md5()` or `hashtext()` are deterministic: same input
always produces same output. This means:

- `public.orders.customer_email` and `public.users.email` masked with
  `fake_email` will produce the same masked value for the same original email
- JOINs between masked tables work correctly
- UNIQUE constraints are preserved (with negligible collision probability)
- Adding a secret salt prevents rainbow-table attacks

## Data Structures

```c
/* masking.h */

typedef enum MaskingStrategy
{
    MASK_CONSTANT,
    MASK_NULL,
    MASK_ZERO,
    MASK_SCRAMBLE,
    MASK_PARTIAL,
    MASK_FAKE_EMAIL,
    MASK_FAKE_NAME,
    MASK_RANDOM_INT,
    MASK_RANDOM_DATE,
    MASK_NOISE,
    MASK_DEFAULT,
    MASK_SQL,
    MASK_FUNCTION
} MaskingStrategy;

typedef struct MaskingRule
{
    char           *schema_pattern;   /* "*" or "default" or exact name */
    char           *table_pattern;
    char           *column_pattern;
    char           *type_pattern;     /* for @type rules, else NULL */
    MaskingStrategy strategy;
    char           *str_param;        /* value=, expr=, name= */
    int             int_param;        /* last=, min=, max=, variance= */
    int             int_param2;       /* second numeric param (max for range) */
} MaskingRule;

typedef struct MaskingConfig
{
    MaskingRule    *rules;
    int             nrules;
    int             capacity;
    char           *salt;
} MaskingConfig;
```

## Integration Points in pg_dump

### 1. DumpOptions (pg_backup.h)

```c
typedef struct _dumpOptions
{
    /* ... existing fields ... */
    struct MaskingConfig *masking;   /* NULL if no masking */
} DumpOptions;
```

### 2. Command-line parsing (pg_dump.c main())

```c
case 13:  /* --masking */
    dopt.masking = parseMaskingConfig(optarg);
    if (!dopt.masking)
        exit_nicely(1);
    break;
```

No `CREATE FUNCTION`, no `executeMaintenanceCommand`, no database modifications.

### 3. COPY method (dumpTableData_copy)

```c
if (tdinfo->filtercond || tbinfo->relkind == RELKIND_FOREIGN_TABLE
    || dopt->masking)
{
    appendPQExpBufferStr(q, "COPY (SELECT ");
    nfields = 0;
    for (int i = 0; i < tbinfo->numatts; i++)
    {
        if (tbinfo->attisdropped[i])
            continue;
        if (nfields > 0)
            appendPQExpBufferStr(q, ", ");

        char *expr = dopt->masking
            ? getMaskingExpression(dopt->masking,
                                   tbinfo->dobj.namespace->dobj.name,
                                   tbinfo->dobj.name,
                                   tbinfo->attnames[i],
                                   tbinfo->atttypnames[i])
            : NULL;

        if (expr)
        {
            appendPQExpBuffer(q, "%s AS %s", expr, fmtId(tbinfo->attnames[i]));
            pfree(expr);
        }
        else
            appendPQExpBufferStr(q, fmtId(tbinfo->attnames[i]));

        nfields++;
    }
    appendPQExpBuffer(q, " FROM %s %s) TO stdout;",
                      fmtQualifiedDumpable(tbinfo),
                      tdinfo->filtercond ? tdinfo->filtercond : "");
}
```

### 4. INSERT method (dumpTableData_insert)

Same pattern — in the column loop, call `getMaskingExpression()` and substitute.

### 5. What does NOT change

- Archive/ArchiveHandle — untouched
- parallel.c — untouched (DumpOptions is already cloned on fork)
- pg_backup_custom.c / pg_backup_tar.c — untouched
- No `executeMaintenanceCommand` / `CREATE FUNCTION` on the server

## Rule Matching Algorithm

Rules are evaluated in order of specificity (most specific wins):

1. Exact match: `public.users.email`
2. Table wildcard: `public.*.email`
3. Schema wildcard: `*.users.email`
4. Full wildcard: `*.*.email`
5. Default fallback: `default.default.email`
6. Type-based: `@text`
7. Global default: `default.default.default`

For multiple matches at the same specificity level, the first rule in the
config file wins.

Implementation: ~30 lines — iterate rules array, score each by specificity,
return highest-scoring match.

## Validation: --masking-check

```
$ pg_dump --masking=rules.conf --masking-check mydb

Masking plan:
  public.users.email          -> fake_email (text)
  public.users.phone          -> partial last=4 (varchar)
  public.users.ssn            -> constant 'XXX-XX-XXXX' (varchar)
  public.orders.customer_name -> scramble (text)

Warnings:
  Rule 'public.payments.card_number' matched no columns
  Column 'public.audit.user_ip' (inet) has no masking rule

4 columns in 2 tables will be masked.
```

Implementation: after `getTableData()`, iterate all tables+columns, match
against rules, print report to stderr, exit.

## Verbose Logging

With `--verbose`, emit per-table masking info:

```
pg_dump: info: masking public.users.email -> fake_email
pg_dump: info: masking public.users.phone -> partial(last=4)
pg_dump: info: dumping contents of table "public.users"
```

## Comparison: Current vs Proposed

| Aspect                    | Current Implementation        | Proposed Design                    |
|---------------------------|-------------------------------|------------------------------------|
| Modifies database         | Yes (CREATE FUNCTION/SCHEMA)  | No                                 |
| Works on read replicas    | No                            | Yes                                |
| Config format             | Custom DSL with { }           | One line = one rule                |
| Parser complexity         | ~300 lines, hand-written SM   | ~80 lines, line-based              |
| Built-in strategies       | 1 (default)                   | 12+                                |
| Deterministic masking     | No                            | Yes (md5/hashtext + salt)          |
| Preserves UNIQUE/FK       | No (XXXX for all rows)        | Yes                                |
| Preserves CHECK           | No                            | Yes (partial, noise, random_int)   |
| Pattern matching          | Only "default" keyword        | *, default, @type                  |
| Config validation         | No                            | --masking-check                    |
| SQL injection             | Possible                      | fmtId() for all identifiers        |
| Memory safety             | Leaks, buffer overflows       | PQExpBuffer + pfree                |
| Global state              | static MaskingMap*            | In DumpOptions                     |
| Parallel dump             | Works (inherited global)      | Works (DumpOptions cloned)         |
| Code size                 | ~1667 lines added             | ~900 lines estimated               |

## Estimated Code Size

| File                      | Lines   | Description                          |
|---------------------------|---------|--------------------------------------|
| masking.h                 | ~60     | Structs, enums, prototypes           |
| masking.c                 | ~400    | Parser, rule matching, SQL gen       |
| pg_dump.c                 | ~50     | Option parsing, 2 integration points |
| pg_backup.h               | ~3      | masking field in DumpOptions         |
| pg_dump.sgml              | ~80     | Documentation                        |
| 011_dump_masking.pl       | ~300    | TAP tests                            |
| **Total**                 | **~900**|                                      |

## Migration Path

The `--masking` option name is preserved. The config file format changes
(breaking change), but since the feature is not yet released/merged, this
is acceptable.

## Security Considerations

1. **No SQL injection** — all identifiers go through `fmtId()`, strategy
   expressions are generated from validated enums, not user strings.
2. **`sql` strategy** — user-provided expressions are passed as-is, which is
   acceptable because pg_dump runs under the user's own database credentials.
3. **Salt confidentiality** — the salt is in the config file, not in the dump
   output. Config file should have restricted permissions (0600).
4. **No server modifications** — no risk of leaving artifacts on production.

## Industry Landscape

Research of existing solutions informed this design. Key findings:

### PostgreSQL Anonymizer (Dalibo) — dominant open-source solution
- Rules as `SECURITY LABEL` in the catalog — declarative, travels with schema
- Rich function library: `fake_*`, `pseudo_*`, `partial()`, `noise()`, `hash()`
- Deterministic masking via `pseudo_*` (seed + salt) preserves FK integrity
- **Requires server-side extension installation** — not suitable for pg_dump core

### Greenmask — modern alternative
- YAML config, drop-in pg_dump replacement
- Every transformer supports hash engine (deterministic) or random engine
- Dynamic parameters allow referencing other column values
- **Standalone tool** — does not integrate into pg_dump itself

### Oracle Data Masking — gold standard for constraints
- **Automatic UNIQUE preservation** via decimal arithmetic
- **Automatic FK preservation** via Application Data Modeling (ADM)
- Group/compound masking for related columns
- Our design achieves FK/UNIQUE preservation via deterministic hashing (simpler
  but effective for the dump use case)

### SQL Server Dynamic Data Masking — cautionary example
- **Cannot mask PK, FK, or UNIQUE columns at all** — too restrictive
- Only 5 built-in strategies
- Our design avoids this limitation by using deterministic expressions

### Myanon (MySQL) — elegant stream approach
- HMAC-SHA256 for deterministic masking — same idea as our `md5(salt || col)`
- Stream processor for mysqldump output
- Validates that our approach to deterministic masking is proven in practice

### Key Architectural Insight

All mature solutions face the tension: **randomization improves privacy but
breaks constraints; determinism preserves constraints but weakens privacy.**
Our design resolves this by defaulting to deterministic strategies (md5/hashtext)
with a user-provided salt, giving both constraint safety and configurable
privacy strength.

## Future Extensions

- `--masking-security-labels`: read rules from `SECURITY LABEL` (PostgreSQL
  Anonymizer compatibility)
- `--masking-format=json`: alternative config format for programmatic generation
- Integration with `pg_dumpall` (apply same rules across all databases)
- `pg_restore --masking`: mask during restore instead of dump
- Compound masking: mask related columns together (city+state+zip)
- Auto-discovery: detect PII columns by name patterns and suggest rules

# Code Review: pg_dump --masking

**Reviewed files**: `masking.h`, `masking.c`, `pg_dump.c` (diff), `011_dump_masking.pl`,
`pg_dump.sgml` (diff)

**Commits**: `a10183e` (initial), `e5c9738` (fixes), `3335800` (passing autotests)

---

## CRITICAL — Must Fix Before Merge

### C1. Modifies production database at dump time

**masking.c:770-878, pg_dump.c:createMaskingFunctions()**

```c
result = executeMaintenanceCommand(conn, default_functions(), true);
```

`createMaskingFunctions()` executes `CREATE SCHEMA IF NOT EXISTS _masking_function`
and 28 `CREATE OR REPLACE FUNCTION` statements **on the live production database**.
This is the single most serious issue:

- **Read-only replicas**: fails immediately — `pg_dump` cannot write to standby
- **Change management**: unauthorized DDL on production violates SOC2/PCI/HIPAA
  policies
- **Artifact pollution**: leaves `_masking_function` schema and functions behind
  after dump completes — no cleanup
- **Race conditions**: concurrent `pg_dump --masking` calls may collide on the
  schema/function creation
- **Permissions**: requires `CREATE` privilege, which dump-only roles typically
  don't have

**Fix**: Replace server-side functions with inline SQL expressions in the
`SELECT` list. E.g. instead of `_masking_function.default(col)`, emit
`'XXXX'::text` directly in the `COPY (SELECT ...) TO` query. See design
document section "Built-in Masking Strategies".

---

### C2. SQL injection via unescaped identifiers

**masking.c:512, 501, 510**

```c
strcpy(col_with_func, psprintf("%s.%s", schema_name, function_name));
strcpy(col_with_func, psprintf("%s(%s)", col_with_func, column_name));
```

Schema names, function names, and column names from the config file are
interpolated directly into SQL without `fmtId()` quoting. A malicious config
file can inject arbitrary SQL:

```
schema1 {
    table1 {
        col1: ")); DROP TABLE users; --"
    }
}
```

This will be concatenated into the query verbatim.

**Also in pg_dump.c** (`maskingColumns` call at dumpTableData_copy):
```c
maskingColumns(tbinfo->dobj.namespace->dobj.name, tbinfo->dobj.name,
               pg_strdup(column_list), masking_map, &q);
```
Column names extracted via `strtok` are passed through without escaping.

**Fix**: All identifiers must go through `fmtId()`. All user-supplied string
values must be properly quoted with `appendStringLiteralConn()` or similar.

---

### C3. `char` return type for `fgetc()` — undefined behavior

**masking.c:199**

```c
char c = fgetc(fin);
```

`fgetc()` returns `int`, not `char`. On platforms where `char` is unsigned,
`EOF` (-1) is indistinguishable from `0xFF` (valid byte ÿ in Latin-1). On
platforms where `char` is signed, it works by accident but is still technically
UB. This affects `readNextSymbol()`, `readName()`, `readWord()`, and all
callers.

Same issue at **masking.c:568**:
```c
char c;
...
c = tolower(getc(fin));
```

And in `ParserState` struct (**masking.h:71**):
```c
char c;  /* should be int */
```

**Fix**: Change all `fgetc`/`getc` return variables and related function
signatures to `int`.

---

### C4. Buffer overflow in `concatFunctionAndColumn`

**masking.c:496-513**

```c
void
concatFunctionAndColumn(char *col_with_func, char *schema_name,
                        char *column_name, char *function_name)
{
    strcpy(col_with_func, psprintf("%s.%s", schema_name, function_name));
    strcpy(col_with_func, psprintf("%s(%s)", col_with_func, column_name));
}
```

The destination buffer `col_with_func` is allocated as `COL_WITH_FUNC_SIZE`
(= `3 * 64 + 3` = 195 bytes). But `psprintf` allocates a new string of
arbitrary length, then `strcpy` copies it back into the fixed-size buffer.
If `schema_name + function_name + column_name > 192`, this overflows.

Also, `psprintf` allocates memory that is never freed (leaked on every call).

**Fix**: Use `PQExpBuffer` to build the expression, or at minimum use
`snprintf` with bounds checking.

---

### C5. Memory leaks throughout

Multiple systematic leak patterns:

1. **masking.c:385**: `getFullRelName()` returns `psprintf`-allocated string,
   used as map key in `setMapValue()` which calls `copy_string()` — original
   never freed.

2. **masking.c:524**: `getFullRelName()` called in `addFunctionToColumn()` — each
   of the 4 fallback lookups allocates and leaks a string.

3. **masking.c:501, 510, 512**: `psprintf()` return values passed to `strcpy()`
   and never freed.

4. **masking.c:553-557**: `removeQuotes()` allocates `pg_malloc(PATH_MAX + 1)`
   to remove 2 chars — overengineered and leaks if `strcpy` fails.

5. **pg_dump.c:maskingColumns** (via masking.c:706-730): `pg_strdup(column_list)`
   passed to `strtok` — original never freed. `pg_malloc(COL_WITH_FUNC_SIZE)` at
   line 709 is allocated but immediately overwritten at line 714 by the return
   value of `addFunctionToColumn()` — original allocation leaked on every
   iteration.

**Fix**: Use `PQExpBuffer` for string construction (standard in pg_dump).
Free `psprintf` results. Consider arena/pool allocation for rule storage.

---

## HIGH — Significant Issues

### H1. `strtok` destroys input, incorrect column parsing

**masking.c:708**

```c
char *current_column_name = strtok(column_list, " ,()");
```

`strtok` modifies the input string in-place. The `column_list` parameter comes
from `pg_dump`'s internal column list format `(col1, col2, col3)`. After
`strtok`, the original string is destroyed. Moreover, `strtok` splits on
individual characters — if a column name contains any of ` ,()` (which is
valid for quoted identifiers), parsing is incorrect.

Example: column named `"amount (USD)"` would be split into `"amount`, `USD)"`.

**Fix**: Iterate over the `TableInfo->attnames[]` array directly instead of
parsing a string representation.

---

### H2. Global mutable state breaks encapsulation

**pg_dump.c:131-132**

```c
static SimpleStringList masking_func_query_path = {NULL, NULL};
static MaskingMap *masking_map;
```

Masking state is stored in file-level globals. This conflicts with pg_dump's
established pattern of threading all state through `DumpOptions`. Problems:

- `masking_map` is accessed from `dumpTableData_copy` and
  `dumpTableData_insert` without being part of the callback context
- In parallel dump mode, these globals are inherited by `fork()` but cannot
  be safely modified post-fork
- Breaks the design pattern established by every other pg_dump option

**Fix**: Add `MaskingConfig *masking` field to `DumpOptions`. Access via
`dopt->masking` in dump functions.

---

### H3. `MaskingMap` is O(n) linear scan

**masking.c:55-68**

```c
int
getMapIndexByKey(MaskingMap *map, char *key)
{
    int index = 0;
    while (map->data[index] != NULL)
    {
        if (strcmp(map->data[index]->key, key) == 0)
            return index;
        index++;
    }
    return -1;
}
```

Every column lookup requires scanning the entire map. For a database with
1000 columns and 100 rules, this is 4 lookups × 100 comparisons × 1000
columns = 400,000 string comparisons. Should use a hash table (`simplehash.h`
is available in pg_dump).

Additionally, the scan at line 59 doesn't check `index < map->size`, so it
reads past the end of the array if the key is not found and the array is
fully packed.

---

### H4. `default` as function name — reserved word collision

**masking.c:770-771**

```c
"CREATE SCHEMA IF NOT EXISTS _masking_function;\n"
"CREATE OR REPLACE FUNCTION _masking_function.default(in text, out text)\n"
```

`default` is a SQL reserved word. Using it as a function name requires quoting
(`"default"`). While PostgreSQL allows it in some contexts, it's fragile and
confusing. The test at line 185 checks for the *unquoted* name, suggesting
this works by accident:

```perl
ok($dump =~ qr/CREATE FUNCTION _masking_function\."default"\(text/, ...)
```

---

### H5. `readWord` uses `strncat` with char pointer — UB

**masking.c:578**

```c
strncat(word, &c, 1);
```

`&c` is a pointer to a single `char` variable on the stack. `strncat`
expects a null-terminated string. `&c` is not null-terminated — the byte
after `c` on the stack is arbitrary. While `n=1` limits the copy, the source
string is still read looking for the null terminator, which is a read past
the end of `c`.

Same issue at **masking.c:276**:
```c
strncat(rel_name, single_char_str, 1);
```
(this one is correct — `single_char_str` is a 2-byte array initialized to `"\0\0"`)

---

### H6. No cleanup of server-side objects on failure or success

When `pg_dump --masking` completes (or crashes), the `_masking_function`
schema and all created functions remain on the server. There is no `DROP
SCHEMA _masking_function CASCADE` in any code path:

- Normal exit: artifacts left behind
- Error exit: artifacts left behind
- `SIGINT`/`SIGTERM`: artifacts left behind

For custom functions from files, `CREATE FUNCTION` runs in the user's schema —
even worse, as these may conflict with existing functions.

---

### H7. Config file allows arbitrary SQL execution on server

**masking.c:658-675, pg_dump.c:createMaskingFunctions()**

The config file can reference external `.sql` files that are read and executed
verbatim via `executeMaintenanceCommand()`. The only validation is checking
that the file starts with `CREATE [OR REPLACE] FUNCTION` — trivially
bypassed:

```sql
CREATE OR REPLACE FUNCTION evil(text) RETURNS text AS $$
BEGIN
  EXECUTE 'DROP DATABASE production';
  RETURN $1;
END;
$$ LANGUAGE plpgsql;
```

The comment in the code acknowledges this:
> "We don't check the full script because we are guessing that this script
> will be run by users who has access to run them and will not harm theirs own
> data"

This is inadequate. A DBA might receive a masking config file from a
colleague/CI system without reviewing the referenced SQL files.

---

## MEDIUM — Code Quality Issues

### M1. Inconsistent coding style (PostgreSQL conventions)

PostgreSQL C code follows specific conventions:
- Tabs for indentation (not spaces)
- Opening brace on its own line for function definitions
- No C99/C11 features without `pg_` wrappers
- `pg_log_error`/`pg_log_warning` format strings should not have trailing period

Throughout `masking.c` and the `pg_dump.c` diff:
- Mixed tabs and spaces (e.g., masking.c:340 uses mixed tab+spaces)
- `{` placement inconsistent (lines 74, 84, 93 have K&R style; line 46 has
  Allman)
- `allocate_memory` and `copy_string` (masking.c:73-90) are unnecessary
  wrappers around `pg_malloc` — `pg_malloc` already exits on failure, so
  the NULL checks are dead code
- pg_dump.c diff uses spaces where surrounding code uses tabs

---

### M2. `extractFunctionNameFromQueryFile` return value is inverted

**masking.c:649**

```c
return func_name[0] != '\0';  /* returns 0 if name found, 1 if not found */
```

But the caller at line 665:
```c
if (extractFunctionNameFromQueryFile(func_path, func_name) != 0)
```
...treats non-zero as success. So the function returns 1 on success and 0 on
failure — the opposite of standard C/POSIX convention (0 = success). The
comment says "If we got a function name, then - return 0, else - return 1" but
the code does the opposite (`!= '\0'` returns 1 when name is found).

This works by accident (the caller checks `!= 0`), but is confusing and
error-prone.

---

### M3. `readQueryForCreatingFunction` uses `realloc` instead of `pg_realloc`

**masking.c:697**

```c
query = realloc(query, sizeof(char) * (fsize + 1));
```

All other allocations use `pg_malloc`/`pg_realloc`. Mixing `realloc` with
`pg_malloc` is incorrect — `pg_malloc` may use a different allocator. Also,
`fread` return value is not checked, and no NULL check on `realloc`.

---

### M4. `isSpace` treats EOF as whitespace

**masking.c:138-140**

```c
bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == EOF;
}
```

EOF should not be treated as space. This masks EOF conditions in the parser —
when the file ends unexpectedly, the parser may continue processing instead of
reporting an error. Combined with C3 (char instead of int), EOF detection is
fundamentally broken.

---

### M5. `isTerminal` and `isSpace` shadow standard library names

These function names are too generic and conflict with common conventions.
PostgreSQL style would be `isMaskTerminal` or similar with a prefix.

---

### M6. `REL_SEP` is a non-const global variable

**masking.c:24**

```c
char REL_SEP = '.';
```

Should be `static const char` or `#define`. As a non-const non-static global,
it pollutes the symbol table and could theoretically be modified.

---

### M7. `my $dump = slurp_file(...)` redeclared in tests

**011_dump_masking.pl:183**

```perl
my $dump = slurp_file($plainfile_insert);
```

`$dump` was already declared at line 161 with `my`. Redeclaring with `my` in
the same scope is a Perl warning under `strict`. Should use `$dump = ...`
(without `my`) for the second assignment.

---

### M8. `BF_BUFFER_SIZE` defined but never used

**masking.h:28**

```c
#define BF_BUFFER_SIZE 1024
```

This constant is defined but never referenced anywhere in the code.

---

### M9. `ParserState` struct members declared but duplicated

**masking.h:65-73**

```c
typedef struct {
    char *schema_name;
    char *table_name;
    char *column_name;
    char *func_name;
    bool skip_reading;
    char c;
    struct MaskingDebugDetails md;
} ParserState;
```

This struct is defined in the header but never used. The actual parser in
`readMaskingPatternFromFile` declares these as local variables instead.

---

### M10. Inconsistent error message quoting

**masking.c:607**

```c
pg_log_warning("Problem with file \'%s\".", filename);
```

Mixed quote types: escaped single quote `\'` on the left, double quote `\"`
on the right. Same issue at **masking.c:688**. Should be consistent
(PostgreSQL convention: double quotes for identifiers, single quotes for
values).

---

## LOW — Minor Issues

### L1. `--masking` help text formatting

**pg_dump.c:1055**

```c
printf(_("  --masking    				 data masking, helps with hiding sensitive data\n"));
```

Uses tabs instead of spaces for alignment, doesn't match the alignment style
of surrounding help entries. The description should follow the pattern:
`"  --masking=FILENAME         mask column data using rules from FILENAME\n"`.

---

### L2. Documentation incomplete

The `pg_dump.sgml` addition documents the config format but:
- Does not describe what `default` means as a function name
- Does not warn about server-side modifications
- Does not list supported data types for the default function
- Missing `<option>` tag cross-references to other relevant options
- No mention of interaction with `--jobs` (parallel dump)

---

### L3. Test file hardcodes test numbering

**011_dump_masking.pl** throughout:

```perl
"1. Run masking without options"
"2. [Default function] Field1 was masked"
```

TAP test descriptions should not include manual numbers. `Test::More`
provides automatic numbering. If a test is added in the middle, all
subsequent numbers must be renumbered.

---

### L4. Test coverage gaps

- No test for UNIQUE constraint violations (all masked values become 'XXXX')
- No test for FK consistency across tables
- No test for parallel dump correctness (`--jobs` test only checks exit code)
- No test for `--format=custom` or `--format=tar`
- No negative test for SQL injection via config file
- No test for binary data types (bytea)
- No test for NULL handling
- No test for tables with no masking rules (regression)
- No test for empty tables

---

### L5. `dumpjobfile` variable unused

**011_dump_masking.pl:21**

```perl
my $dumpjobfile = "$backupdir/parallel/toc.dat'";
```

Contains a stray `'` at the end and is never used.

---

### L6. `close_brace_counter` not reset properly

**masking.c:451**

`close_brace_counter` is incremented when `}` is encountered in `WAIT_COMMA`
state, but it's only reset at line 471 when transitioning to a new
schema/table name. If the parser encounters `}}` followed by EOF (no new
schema), `close_brace_counter` is never validated, and `brace_counter` may
be non-zero at exit without an error.

---

## Summary

| Severity | Count | Key Themes                                           |
|----------|-------|------------------------------------------------------|
| CRITICAL | 5     | Server mutation, SQL injection, UB, buffer overflow   |
| HIGH     | 7     | Memory leaks, globals, O(n) map, no cleanup          |
| MEDIUM   | 10    | Style, logic errors, dead code                       |
| LOW      | 6     | Docs, tests, minor formatting                        |
| **Total**| **28**|                                                      |

### Recommendation

**Do not merge in current form.** The critical issues (C1: server modification,
C2: SQL injection, C3: undefined behavior) individually justify a rewrite.
Combined with the architectural issues (global state, no cleanup, O(n) lookups),
the implementation needs a fundamental redesign rather than incremental fixes.

The `MASKING_DESIGN.md` document in this branch provides a complete alternative
design that addresses all identified issues while reducing code complexity.

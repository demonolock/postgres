# Copyright (c) 2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;
my $inputfile;

my $node      = PostgreSQL::Test::Cluster->new('main');
my $port      = $node->port;
my $backupdir = $node->backup_dir;
my $plainfile = "$backupdir/plain_copy.sql";
my $plainfile_insert = "$backupdir/plain_insert.sql";
my $dumpfile = "$backupdir/options_plain.sql";

$node->init;
$node->start;

# Generate test schemas and tables
$node->safe_psql('postgres', 'CREATE SCHEMA schema1;');
$node->safe_psql('postgres', 'CREATE SCHEMA schema2;');
$node->safe_psql('postgres', 'CREATE SCHEMA schema3;');
$node->safe_psql('postgres', 'CREATE SCHEMA schema4;');
$node->safe_psql('postgres', 'CREATE SCHEMA schema5;');

$node->safe_psql('postgres', "CREATE TABLE schema1.users(id serial PRIMARY KEY, name varchar, email varchar);");
$node->safe_psql('postgres', "CREATE TABLE schema2.orders(id serial PRIMARY KEY, customer_email varchar, amount numeric);");
$node->safe_psql('postgres', "CREATE TABLE schema3.sensitive(id serial PRIMARY KEY, ssn varchar, birthdate date, salary numeric);");
$node->safe_psql('postgres', "CREATE TABLE schema4.mixed(id serial PRIMARY KEY, phone varchar, notes text, score integer);");
$node->safe_psql('postgres', "CREATE TABLE schema5.timestamps_tbl(id serial PRIMARY KEY, created_at timestamptz, label varchar);");

# Insert test data
$node->safe_psql('postgres', "INSERT INTO schema1.users(name, email) VALUES('Alice', 'alice\@example.com');");
$node->safe_psql('postgres', "INSERT INTO schema1.users(name, email) VALUES('Bob', 'bob\@example.com');");
$node->safe_psql('postgres', "INSERT INTO schema2.orders(customer_email, amount) VALUES('alice\@example.com', 99.50);");
$node->safe_psql('postgres', "INSERT INTO schema3.sensitive(ssn, birthdate, salary) VALUES('123-45-6789', '1985-03-15', 75000);");
$node->safe_psql('postgres', "INSERT INTO schema4.mixed(phone, notes, score) VALUES('+1-555-0100', 'some notes', 42);");
$node->safe_psql('postgres', "INSERT INTO schema5.timestamps_tbl(created_at, label) VALUES('2024-06-15 10:30:00+00', 'test');");

# Also create an existing masking function on the server to test MASK_FUNCTION
$node->safe_psql('postgres', "CREATE SCHEMA mask_utils;");
$node->safe_psql('postgres', "
	CREATE FUNCTION mask_utils.mask_phone(val text) RETURNS text AS \$\$
	  SELECT regexp_replace(val, '\\d', 'X', 'g');
	\$\$ LANGUAGE SQL;
");


#########################################
# Test 1: constant strategy
#########################################
open $inputfile, '>', "$tempdir/masking_constant.conf"
  or die "unable to open masking_constant.conf for writing";
print $inputfile "# constant strategy test
schema1.users.email      constant  value=REDACTED
";
close $inputfile;

command_ok(
	['pg_dump', '-p', $port, '-f', $plainfile,
	 "--masking=$tempdir/masking_constant.conf"],
	"constant strategy: pg_dump succeeds");

my $dump = slurp_file($plainfile);
ok($dump =~ qr/^COPY schema1\.users .+ FROM stdin;\n\d+\tAlice\tREDACTED\n/m,
   "constant strategy: email replaced with REDACTED in COPY");
ok($dump =~ qr/^COPY schema2\.orders .+ FROM stdin;\n\d+\talice\@example\.com/m,
   "constant strategy: unmatched table is not masked");


#########################################
# Test 2: default strategy
#########################################
open $inputfile, '>', "$tempdir/masking_default.conf"
  or die "unable to open masking_default.conf for writing";
print $inputfile "schema3.sensitive.ssn         default
schema3.sensitive.birthdate   default
schema3.sensitive.salary      default
";
close $inputfile;

command_ok(
	['pg_dump', '-p', $port, '-f', $plainfile,
	 "--masking=$tempdir/masking_default.conf"],
	"default strategy: pg_dump succeeds");

$dump = slurp_file($plainfile);
ok($dump =~ qr/^COPY schema3\.sensitive .+ FROM stdin;\n\d+\tXXXX\t1900-01-01\t0/m,
   "default strategy: ssn->XXXX, birthdate->1900-01-01, salary->0");


#########################################
# Test 3: fake_email strategy (deterministic)
#########################################
open $inputfile, '>', "$tempdir/masking_fake_email.conf"
  or die "unable to open masking_fake_email.conf for writing";
print $inputfile ".salt = test_salt_123
schema1.users.email            fake_email
schema2.orders.customer_email  fake_email
";
close $inputfile;

command_ok(
	['pg_dump', '-p', $port, '-f', $plainfile,
	 "--masking=$tempdir/masking_fake_email.conf"],
	"fake_email strategy: pg_dump succeeds");

$dump = slurp_file($plainfile);
# Check that emails are masked with the fake pattern
ok($dump =~ qr/user_[0-9a-f]{8}\@masked\.invalid/m,
   "fake_email strategy: email replaced with deterministic fake");
# Check determinism: same original email -> same masked email across tables
my @masked_emails;
while ($dump =~ /user_([0-9a-f]{8})\@masked\.invalid/g)
{
	push @masked_emails, $1;
}
# alice@example.com appears in both tables; its hashes should match
ok(scalar(@masked_emails) >= 3,
   "fake_email strategy: found at least 3 masked emails");
# First Alice email (users table) should match the orders table email
ok($masked_emails[0] eq $masked_emails[2],
   "fake_email strategy: deterministic - same input produces same hash across tables");


#########################################
# Test 4: scramble strategy
#########################################
open $inputfile, '>', "$tempdir/masking_scramble.conf"
  or die "unable to open masking_scramble.conf for writing";
print $inputfile "schema1.users.name   scramble
";
close $inputfile;

command_ok(
	['pg_dump', '-p', $port, '-f', $plainfile,
	 "--masking=$tempdir/masking_scramble.conf"],
	"scramble strategy: pg_dump succeeds");

$dump = slurp_file($plainfile);
ok($dump =~ qr/^COPY schema1\.users .+ FROM stdin;\n\d+\t[0-9a-f]{32}\t/m,
   "scramble strategy: name replaced with md5 hash");


#########################################
# Test 5: partial strategy
#########################################
open $inputfile, '>', "$tempdir/masking_partial.conf"
  or die "unable to open masking_partial.conf for writing";
print $inputfile "schema4.mixed.phone   partial   last=4
";
close $inputfile;

command_ok(
	['pg_dump', '-p', $port, '-f', $plainfile,
	 "--masking=$tempdir/masking_partial.conf"],
	"partial strategy: pg_dump succeeds");

$dump = slurp_file($plainfile);
ok($dump =~ qr/XXXX+0100/m,
   "partial strategy: phone number partially masked, last 4 visible");


#########################################
# Test 6: null and zero strategies
#########################################
open $inputfile, '>', "$tempdir/masking_null_zero.conf"
  or die "unable to open masking_null_zero.conf for writing";
print $inputfile "schema3.sensitive.ssn     null
schema3.sensitive.salary  zero
";
close $inputfile;

command_ok(
	['pg_dump', '-p', $port, '-f', $plainfile,
	 "--masking=$tempdir/masking_null_zero.conf"],
	"null/zero strategies: pg_dump succeeds");

$dump = slurp_file($plainfile);
ok($dump =~ qr/^COPY schema3\.sensitive .+ FROM stdin;\n\d+\t\\N\t/m,
   "null strategy: ssn replaced with NULL");


#########################################
# Test 7: wildcard patterns
#########################################
open $inputfile, '>', "$tempdir/masking_wildcard.conf"
  or die "unable to open masking_wildcard.conf for writing";
print $inputfile "# Wildcard: mask email everywhere
*.*.email              fake_email
*.*.customer_email     fake_email
";
close $inputfile;

command_ok(
	['pg_dump', '-p', $port, '-f', $plainfile,
	 "--masking=$tempdir/masking_wildcard.conf"],
	"wildcard patterns: pg_dump succeeds");

$dump = slurp_file($plainfile);
ok($dump =~ qr/user_[0-9a-f]{8}\@masked\.invalid/m,
   "wildcard: emails masked across all schemas");
ok($dump !~ qr/alice\@example\.com/,
   "wildcard: no plain emails remain for matched columns");


#########################################
# Test 8: function strategy (existing server function)
#########################################
open $inputfile, '>', "$tempdir/masking_function.conf"
  or die "unable to open masking_function.conf for writing";
print $inputfile "schema4.mixed.phone   function   name=mask_utils.mask_phone
";
close $inputfile;

command_ok(
	['pg_dump', '-p', $port, '-f', $plainfile,
	 "--masking=$tempdir/masking_function.conf"],
	"function strategy: pg_dump succeeds");

$dump = slurp_file($plainfile);
# mask_phone replaces digits with X
ok($dump =~ qr/\+X-XXX-XXXX/m,
   "function strategy: phone masked via server function");


#########################################
# Test 9: sql strategy (inline expression)
#########################################
open $inputfile, '>', "$tempdir/masking_sql.conf"
  or die "unable to open masking_sql.conf for writing";
print $inputfile "schema3.sensitive.birthdate   sql   expr=date_trunc('year',birthdate)::date
";
close $inputfile;

command_ok(
	['pg_dump', '-p', $port, '-f', $plainfile,
	 "--masking=$tempdir/masking_sql.conf"],
	"sql strategy: pg_dump succeeds");

$dump = slurp_file($plainfile);
ok($dump =~ qr/1985-01-01/m,
   "sql strategy: birthdate truncated to year");


#########################################
# Test 10: noise strategy
#########################################
open $inputfile, '>', "$tempdir/masking_noise.conf"
  or die "unable to open masking_noise.conf for writing";
print $inputfile "schema3.sensitive.salary   noise   variance=20
";
close $inputfile;

command_ok(
	['pg_dump', '-p', $port, '-f', $plainfile,
	 "--masking=$tempdir/masking_noise.conf"],
	"noise strategy: pg_dump succeeds");

$dump = slurp_file($plainfile);
# Original salary is 75000; with 20% noise it should be different but in range
ok($dump !~ qr/\t75000\n/m,
   "noise strategy: salary is modified from original");


#########################################
# Test 11: --inserts mode
#########################################
open $inputfile, '>', "$tempdir/masking_inserts.conf"
  or die "unable to open masking_inserts.conf for writing";
print $inputfile "schema1.users.email   constant   value=HIDDEN
schema3.sensitive.ssn default
";
close $inputfile;

command_ok(
	['pg_dump', '-p', $port, '-f', $plainfile_insert,
	 "--masking=$tempdir/masking_inserts.conf",
	 "--inserts"],
	"INSERT mode: pg_dump succeeds with masking");

$dump = slurp_file($plainfile_insert);
ok($dump =~ qr/INSERT INTO schema1\.users VALUES \(\d+, 'Alice', 'HIDDEN'\)/m,
   "INSERT mode: email replaced with constant");
ok($dump =~ qr/INSERT INTO schema3\.sensitive VALUES \(\d+, 'XXXX'/m,
   "INSERT mode: ssn replaced with default");


#########################################
# Test 12: masking with other pg_dump options
#########################################
open $inputfile, '>', "$tempdir/masking_combined.conf"
  or die "unable to open masking_combined.conf for writing";
print $inputfile "schema1.users.email   constant   value=MASKED
";
close $inputfile;

# --data-only
command_ok(
	['pg_dump', '-p', $port, '-f', $dumpfile,
	 "--masking=$tempdir/masking_combined.conf",
	 '--data-only'],
	"combined: --data-only");
ok(slurp_file($dumpfile) =~ qr/MASKED/m,
   "combined: masking works with --data-only");

# --schema
command_ok(
	['pg_dump', '-p', $port, '-f', $dumpfile,
	 "--masking=$tempdir/masking_combined.conf",
	 '--schema=schema1'],
	"combined: --schema");
$dump = slurp_file($dumpfile);
ok($dump =~ qr/MASKED/m,
   "combined: masking works with --schema filter");
ok($dump !~ qr/schema2\.orders/m,
   "combined: --schema filters other schemas");

# --table
command_ok(
	['pg_dump', '-p', $port, '-f', $dumpfile,
	 "--masking=$tempdir/masking_combined.conf",
	 '--table=schema1.users'],
	"combined: --table");
ok(slurp_file($dumpfile) =~ qr/MASKED/m,
   "combined: masking works with --table filter");

# --clean
command_ok(
	['pg_dump', '-p', $port, '-f', $dumpfile,
	 "--masking=$tempdir/masking_combined.conf",
	 '--clean'],
	"combined: --clean");
ok(slurp_file($dumpfile) =~ qr/MASKED/m,
   "combined: masking works with --clean");

# --column-inserts
command_ok(
	['pg_dump', '-p', $port, '-f', $dumpfile,
	 "--masking=$tempdir/masking_combined.conf",
	 '--column-inserts'],
	"combined: --column-inserts");
ok(slurp_file($dumpfile) =~ qr/MASKED/m,
   "combined: masking works with --column-inserts");

# --rows-per-insert
command_ok(
	['pg_dump', '-p', $port, '-f', $dumpfile,
	 "--masking=$tempdir/masking_combined.conf",
	 '--rows-per-insert=10'],
	"combined: --rows-per-insert");
ok(slurp_file($dumpfile) =~ qr/MASKED/m,
   "combined: masking works with --rows-per-insert");

# --no-sync --quote-all-identifiers
command_ok(
	['pg_dump', '-p', $port, '-f', $dumpfile,
	 "--masking=$tempdir/masking_combined.conf",
	 '--no-sync', '--quote-all-identifiers'],
	"combined: --no-sync --quote-all-identifiers");
ok(slurp_file($dumpfile) =~ qr/MASKED/m,
   "combined: masking works with --quote-all-identifiers");


#########################################
# Test 13: no server modification
#########################################
# Verify that no _masking_function schema was created
my $result = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_namespace WHERE nspname = '_masking_function';");
ok($result eq '0',
   "no server modification: _masking_function schema does not exist");


#########################################
# Test 14: random_int strategy
#########################################
open $inputfile, '>', "$tempdir/masking_random_int.conf"
  or die "unable to open masking_random_int.conf for writing";
print $inputfile "schema4.mixed.score   random_int   min=1 max=100
";
close $inputfile;

command_ok(
	['pg_dump', '-p', $port, '-f', $plainfile,
	 "--masking=$tempdir/masking_random_int.conf"],
	"random_int strategy: pg_dump succeeds");

$dump = slurp_file($plainfile);
# Original score is 42; should be replaced with a number in [1,100)
ok($dump =~ qr/^COPY schema4\.mixed .+ FROM stdin;\n\d+\t.*\t.*\t\d+\n/m,
   "random_int strategy: score column contains an integer");


#########################################
# Test 15: fake_name strategy
#########################################
open $inputfile, '>', "$tempdir/masking_fake_name.conf"
  or die "unable to open masking_fake_name.conf for writing";
print $inputfile "schema1.users.name   fake_name
";
close $inputfile;

command_ok(
	['pg_dump', '-p', $port, '-f', $plainfile,
	 "--masking=$tempdir/masking_fake_name.conf"],
	"fake_name strategy: pg_dump succeeds");

$dump = slurp_file($plainfile);
ok($dump =~ qr/name_[0-9a-f]{8}/m,
   "fake_name strategy: name replaced with deterministic pseudonym");
ok($dump !~ qr/\tAlice\t/m,
   "fake_name strategy: original name not present");


#########################################
# Negative tests
#########################################

# Empty filename
command_fails_like(
	['pg_dump', '--masking', ''],
	qr/--masking requires a non-empty filename/,
	"negative: empty filename rejected");

# Non-existent file
command_fails_like(
	['pg_dump', '--masking', '/nonexistent/file.conf'],
	qr/could not open masking file/,
	"negative: non-existent file rejected");

# Missing strategy
open $inputfile, '>', "$tempdir/masking_bad_nostrategy.conf"
  or die "unable to open file for writing";
print $inputfile "schema1.users.email\n";
close $inputfile;

command_fails_like(
	['pg_dump', '--masking', "$tempdir/masking_bad_nostrategy.conf"],
	qr/missing strategy/,
	"negative: missing strategy rejected");

# Bad target format (no dots)
open $inputfile, '>', "$tempdir/masking_bad_nodots.conf"
  or die "unable to open file for writing";
print $inputfile "users_email   constant   value=X\n";
close $inputfile;

command_fails_like(
	['pg_dump', '--masking', "$tempdir/masking_bad_nodots.conf"],
	qr/target must be schema\.table\.column/,
	"negative: target without dots rejected");

# Unknown strategy
open $inputfile, '>', "$tempdir/masking_bad_strategy.conf"
  or die "unable to open file for writing";
print $inputfile "schema1.users.email   nonexistent_strategy\n";
close $inputfile;

command_fails_like(
	['pg_dump', '--masking', "$tempdir/masking_bad_strategy.conf"],
	qr/unknown strategy/,
	"negative: unknown strategy rejected");

# Empty config (no rules)
open $inputfile, '>', "$tempdir/masking_empty.conf"
  or die "unable to open file for writing";
print $inputfile "# only comments, no rules\n";
close $inputfile;

command_fails_like(
	['pg_dump', '--masking', "$tempdir/masking_empty.conf"],
	qr/contains no rules/,
	"negative: empty config rejected");


#########################################
# Test: comments in config
#########################################
open $inputfile, '>', "$tempdir/masking_comments.conf"
  or die "unable to open file for writing";
print $inputfile "# This is a comment
schema1.users.email   constant   value=COMMENTED_TEST
# Another comment
";
close $inputfile;

command_ok(
	['pg_dump', '-p', $port, '-f', $plainfile,
	 "--masking=$tempdir/masking_comments.conf"],
	"comments: pg_dump succeeds with commented config");

$dump = slurp_file($plainfile);
ok($dump =~ qr/COMMENTED_TEST/m,
   "comments: masking works correctly with comments in config");


done_testing();

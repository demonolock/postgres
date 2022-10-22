# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql("postgres", "CREATE TABLE t0(id int, t text)");
$node->safe_psql("postgres", "CREATE TABLE t1(id int, d timestamp)");
$node->safe_psql("postgres", "CREATE TABLE t2(id int, r real)");
$node->safe_psql("postgres", "CREATE TABLE t3(id int)");

$node->safe_psql("postgres", "INSERT INTO t0 SELECT generate_series(1,3) AS id, md5(random()::text) AS t");
$node->safe_psql("postgres", "INSERT INTO t1 SELECT generate_series(1,3) AS id,
															NOW() + (random() * (interval '90 days')) + '30 days' AS d");
$node->safe_psql("postgres", "INSERT INTO t2 SELECT generate_series(1,3) AS id, random() * 100 AS r");
$node->safe_psql("postgres", "INSERT INTO t3 SELECT generate_series(1,3) AS id");

$node->safe_psql("postgres", "CREATE SCHEMA test_schema");

$node->safe_psql("postgres", "CREATE TABLE test_schema.t0(id int)");
$node->safe_psql("postgres", "INSERT INTO test_schema.t0 SELECT generate_series(1,3) AS id");

#masking functions

my %functions = (
	'mask_int' => {
		func_name => 'mask_int',
		code => 'res := -1',
		param_type => 'integer',
		},
	'mask_int_with_schema' => {
		func_name => 'test_schema.mask_int_with_schema',
		code => 'res := -2',
		param_type => 'integer',
		},
	'mask_real' => {
		func_name => 'mask_real',
		code => 'res := -1.5',
		param_type => 'real',
		},
	'mask_text' => {
		func_name => 'mask_text',
		code => 'res := \'*****\'',
		param_type => 'text',
		},
	'mask_timestamp' => {
		func_name => 'mask_timestamp',
		code => 'res := \'1970-01-01 00:00:00\'',
		param_type => 'timestamp',
		},
);

foreach my $function (sort keys %functions)
{
	my $query = sprintf "CREATE OR REPLACE FUNCTION %s (IN elem %s, OUT res %s) RETURNS %s AS
			\$BODY\$
			BEGIN   				
				%s;
				RETURN;
			END
			\$BODY\$ LANGUAGE plpgsql;", $functions{$function}->{func_name}, $functions{$function}->{param_type},
			$functions{$function}->{param_type}, $functions{$function}->{param_type}, $functions{$function}->{code};
	$node->safe_psql("postgres", $query);
}

my %tests = (
    'test_mask_all_ids' => {
		regexp => qr/^
			\QCOPY public.t0 (id, t) FROM stdin;\E\n
			(-1\s*\w*\n){3}
			\Q\.\E\n
			(.|\n)*
			\QCOPY public.t1 (id, d) FROM stdin;\E\n
			(-1\s*\d{4}-\d{2}-\d{2}\ \d{2}:\d{2}:\d{2}\.\d*\n){3}
			\Q\.\E\n
			(.|\n)*
			\QCOPY public.t2 (id, r) FROM stdin;\E\n
			(-1\s*\d*\.\d*\n){3}
			\Q\.\E\n
			(.|\n)*
			\QCOPY public.t3 (id) FROM stdin;\E\n
			(-1\s*\n){3}
			\Q\.\E\n
			(.|\n)*
			\QCOPY test_schema.t0 (id) FROM stdin;\E\n
			(-1\s*\n){3}
			\Q\.\E\
		/xm,
		dump => [
            'pg_dump',
            'postgres',
            '-f', "$tempdir/test_mask_all_ids.sql",
            '--mask-columns', '"id"',
			'--mask-function', 'mask_int']
            },
    'test_mask_some_ids' => {
		regexp => qr/^
            \QCOPY public.t0 (id, t) FROM stdin;\E\n
			(-1\s*\w*\n){3}
			\Q\.\E\n
			(.|\n)*
			\QCOPY public.t1 (id, d) FROM stdin;\E\n
			(-1\s*\d{4}-\d{2}-\d{2}\ \d{2}:\d{2}:\d{2}\.\d*\n){3}
			\Q\.\E\n
			(.|\n)*
			\QCOPY public.t2 (id, r) FROM stdin;\E\n
			1\s*\d*\.\d*\n2\s*\d*\.\d*\n3\s*\d*\.\d*\n
            \Q\.\E\n
			(.|\n)*
			\QCOPY public.t3 (id) FROM stdin;\E\n
			1\s*\n2\s*\n3\s*\n
			\Q\.\E\n
			(.|\n)*
			\QCOPY test_schema.t0 (id) FROM stdin;\E\n
			(-1\s*\w*\n){3}
			\Q\.\E\
        /xm,
		dump => [
            'pg_dump',
            'postgres',
            '-f', "$tempdir/test_mask_some_ids.sql",
            '--mask-columns', '"t0.id, t1.id"',
			'--mask-function', 'mask_int']
            },
    'test_mask_different_types' => {
		regexp => qr/^
            \QCOPY public.t0 (id, t) FROM stdin;\E\n
            1\s*\*{5}\n2\s*\*{5}\n3\s*\*{5}\n
            \Q\.\E\n
			(.|\n)*
			\QCOPY public.t1 (id, d) FROM stdin;\E\n
			1\s*\Q1970-01-01 00:00:00\E\n2\s*\Q1970-01-01 00:00:00\E\n3\s*\Q1970-01-01 00:00:00\E\n
            \Q\.\E\n
			(.|\n)*
			\QCOPY public.t2 (id, r) FROM stdin;\E\n
			1\s*\Q-1.5\E\n2\s*\Q-1.5\E\n3\s*\Q-1.5\E\n
            \Q\.\E\n
			(.|\n)*
			\QCOPY public.t3 (id) FROM stdin;\E\n
			1\s*\n2\s*\n3\s*\n
			\Q\.\E\n
			(.|\n)*
			\QCOPY test_schema.t0 (id) FROM stdin;\E\n
			1\s*\n2\s*\n3\s*\n
			\Q\.\E\
        /xm,
		dump => [
			'pg_dump',
			'postgres',
			'-f', "$tempdir/test_mask_different_types.sql",
			'--mask-columns', 't',
			'--mask-function', 'mask_text',
			'--mask-columns', 'd',
			'--mask-function', 'mask_timestamp',
			'--mask-columns', 'r',
			'--mask-function', 'mask_real']
        },
    'test_mask_ids_with_schema' => {
		regexp => qr/^
			\QCOPY public.t0 (id, t) FROM stdin;\E\n
			(-2\s*\w*\n){3}
			\Q\.\E\n
			(.|\n)*
			\QCOPY public.t1 (id, d) FROM stdin;\E\n
			(-2\s*\d{4}-\d{2}-\d{2}\ \d{2}:\d{2}:\d{2}\.\d*\n){3}
			\Q\.\E\n
			(.|\n)*
			\QCOPY public.t2 (id, r) FROM stdin;\E\n
			(-2\s*\d*\.\d*\n){3}
			\Q\.\E\n
			(.|\n)*
			\QCOPY public.t3 (id) FROM stdin;\E\n
			(-2\s*\n){3}
			\Q\.\E\n
			(.|\n)*
			\QCOPY test_schema.t0 (id) FROM stdin;\E\n
			(-2\s*\n){3}
			\Q\.\E\
		/xm,
		dump => [
            'pg_dump',
            'postgres',
            '-f', "$tempdir/test_mask_ids_with_schema.sql",
            '--mask-columns', 'id',
			'--mask-function', 'test_schema.mask_int_with_schema']
            },
	'test_mask_ids_file' => {
		regexp => qr/^
            \QCOPY public.t0 (id, t) FROM stdin;\E\n
            (-3\s*\w*\n){3}
			\Q\.\E
        /xm,
		dump => [
            'pg_dump',
            'postgres',
            '-f', "$tempdir/test_mask_ids_file.sql",
			'-t', 't0',
            '--mask-columns', 'id',
			'--mask-function', "$tempdir/mask_ids.sql"]
            },
	'test_mask_ids_insert' => {
		regexp => qr/^
			(\QINSERT INTO public.t0 (id, t) VALUES (-1, \E\'\w*\'\Q);\E\n){3}
		/xm,
		dump => [
            'pg_dump',
            'postgres',
            '-f', "$tempdir/test_mask_ids_insert.sql",
			'-t', 't0',
			'--column-insert',
            '--mask-columns', 'id',
			'--mask-function', 'mask_int']
            },
	'test_mask_some_ids_with_schema' => {
		regexp => qr/^
            \QCOPY public.t0 (id, t) FROM stdin;\E\n
			(-1\s*\w*\n){3}
			\Q\.\E\n
			(.|\n)*
			\QCOPY test_schema.t0 (id) FROM stdin;\E\n
			(-1\s*\n){3}
			\Q\.\E\
        /xm,
		dump => [
            'pg_dump',
            'postgres',
            '-f', "$tempdir/test_mask_some_ids_with_schema.sql",
            '--mask-columns', '"t0.id, test_schema.t0.id"',
			'--mask-function', 'mask_int']
            },
);

open my $fileHandle, ">", "$tempdir/mask_ids.sql";
print $fileHandle "f_int\ninteger\nplpgsql\nres := -3;";
close ($fileHandle);

open $fileHandle, ">", "$tempdir/mask_drop_table.sql";
print $fileHandle "f_int\ninteger\nplpgsql\nDROP TABLE t0;\nres := -3;";
close ($fileHandle);

open $fileHandle, ">", "$tempdir/mask_grant.sql";
print $fileHandle "f_int\ninteger\nplpgsql\nres := -3;\nGRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO tester;";
close ($fileHandle);

foreach my $test (sort keys %tests)
{
	$node->command_ok(\@{ $tests{$test}->{dump} },"$test: pg_dump runs");

	my $output_file = slurp_file("$tempdir/${test}.sql");

    ok($output_file =~ $tests{$test}->{regexp}, "$test: should be dumped");
}

#security test - it shouldn't be possible to execute DROP TABLE during dump

$node->command_fails_like(
	['pg_dump', 'postgres', '-f', "$tempdir/test_mask_ids_file.sql",
	 '-t', 't0', '--mask-columns', 'id', '--mask-function', "$tempdir/mask_drop_table.sql" ],
	qr/\Qg_dump: error: Dumping the contents of table "t0" failed: PQgetResult() failed.
pg_dump: detail: Error message from server: ERROR:  cannot execute DROP TABLE in a read-only transaction
CONTEXT:  SQL statement "DROP TABLE t0"
PL\/pgSQL function public.f_int(integer) line 3 at SQL statement
pg_dump: detail: Command was: COPY (SELECT public.f_int(id), t FROM public.t0 ) TO stdout;\E/,
	'trying to drop table during dump');

#security test - it shouldn't be possible to execute GRANT during dump

$node->safe_psql("postgres", "CREATE USER tester");

$node->command_fails_like(
	['pg_dump', 'postgres', '-f', "$tempdir/test_mask_ids_file.sql",
	 '-t', 't0', '--mask-columns', 'id', '--mask-function', "$tempdir/mask_grant.sql" ],
	qr/\Qpg_dump: error: Dumping the contents of table "t0" failed: PQgetResult() failed.
pg_dump: detail: Error message from server: ERROR:  cannot execute GRANT in a read-only transaction
CONTEXT:  SQL statement "GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA public TO tester"
PL\/pgSQL function public.f_int(integer) line 4 at SQL statement
pg_dump: detail: Command was: COPY (SELECT public.f_int(id), t FROM public.t0 ) TO stdout;\E/,
	'trying to drop table during dump');


done_testing();

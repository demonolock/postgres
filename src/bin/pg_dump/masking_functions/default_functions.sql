create if not exists schema _masking_function;
CREATE FUNCTION default(in text, out text)
    AS $$ SELECT $1 || ' default' $$
              LANGUAGE SQL;

CREATE FUNCTION default(in int, out text)
    AS $$ SELECT $1 || ' default' $$
              LANGUAGE SQL;

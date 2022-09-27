create if not exists schema _masking_function;
create or replace function _masking_function.default(char) returns text
    AS './masking_functions.c', 'default_char'
    LANGUAGE C;
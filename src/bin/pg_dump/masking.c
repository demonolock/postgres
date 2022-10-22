/*-------------------------------------------------------------------------
 *
 * masking.c
 *
 *	Masking functionality for pg_dump
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/masking.c
 *
 *-------------------------------------------------------------------------
 */

#include "masking.h"

/*
 * addFuncToDatabase - parses file specified in command line, executes query from it
 * adding masking function to database
 */

static void formMaskingLists(DumpOptions* dopt)
{
	/* needed for masking */
	SimpleStringListCell *mask_func_cell;
	SimpleStringListCell *mask_columns_cell;
	SimplePtrListCell	 *mask_column_info_cell;
	char		*column_name_buffer;
	char		*table_name_buffer;
	char		*schema_table_name_buffer;
	char		*func_name_buffer;
	char		*schema_name_buffer;
	FILE 		*mask_func_file;
	PGconn 		*connection;
	/* 256 is pretty arbitrary, but it is enough for dbname, host, port and user*/
	char		*conn_params = (char*) pg_malloc(256 * sizeof(char)); 

    /*
	* Add all columns and functions to list of MaskColumnInfo structures,
	*/

	mask_func_cell = mask_func_list.head;
	mask_columns_cell = mask_columns_list.head;

	while (mask_columns_cell && mask_func_cell)
	{
		char* func = mask_func_cell->val;
		char* column = strtok(mask_columns_cell->val, " ,\'\"");
		char* table = (char*) pg_malloc(128 * sizeof(char)); /*enough to store schema_name.table_name (63 + 1 + 63 + 1)*/
		char* schema = (char*) pg_malloc(64 * sizeof(char)); /*enough to store schema name (63 + 1)*/
		while (column != NULL)
		{
			MaskColumnInfo* new_mask_column = (MaskColumnInfo*) pg_malloc(sizeof(MaskColumnInfo));
			new_mask_column->column = column;
			new_mask_column->table = table;
			new_mask_column->func = func;
			new_mask_column->schema = schema;
			simple_ptr_list_append(&mask_column_info_list, new_mask_column);
			table = (char*) pg_malloc(128 * sizeof(char));
			column = strtok(NULL, " ,\'\"");
		}
		mask_columns_cell = mask_columns_cell->next;
		mask_func_cell = mask_func_cell->next;
	}

	/*
	* If there is not enough params of one type throw error
	*/

	if (mask_columns_cell != NULL || mask_func_cell != NULL)
		pg_fatal("amount of --mask-columns and --mask-function doesn't match");

	/*
	* Extract tablenames from list of columns - done here so that strtok isn't
	* disturbed in previous cycle
	*/

	mask_column_info_cell = mask_column_info_list.head;

	while (mask_column_info_cell != NULL)
	{
		MaskColumnInfo* cur_mask_column_info = (MaskColumnInfo*) mask_column_info_cell->ptr;
		schema_table_name_buffer = strtok(cur_mask_column_info->column, ".");
		table_name_buffer = strtok(NULL, ".");
		column_name_buffer = strtok(NULL, ".");
		if (table_name_buffer == NULL) /* found column without tablename */
		{
			strcpy(cur_mask_column_info->table, "");
			strcpy(cur_mask_column_info->column, schema_table_name_buffer);
		}
		else
			if (column_name_buffer == NULL) /* name of schema for table isn't specified */
			{
				strcpy(cur_mask_column_info->table, schema_table_name_buffer);
				strcpy(cur_mask_column_info->column, table_name_buffer);
			}
			else
			{
				strcat(schema_table_name_buffer, table_name_buffer);
				strcpy(cur_mask_column_info->table, schema_table_name_buffer);
				strcpy(cur_mask_column_info->column, column_name_buffer);	
			}
		mask_column_info_cell = mask_column_info_cell->next;
	}

	/*
	* Check if --mask-function is a name of function or a filepath
	* A connection is opened before processing any functions; 
	* If a filepath is found - add function through connection;
	* Connection is closed when all functions are processed 
	*/

	mask_column_info_cell = mask_column_info_list.head;

	/* Establishing connection to execute CREATE FUNCTION script */
	strcpy(conn_params, "");
	if(dopt->cparams.override_dbname)
		conn_params = psprintf("%s dbname=%s", conn_params, dopt->cparams.override_dbname);
	else
		if(dopt->cparams.dbname)
			conn_params = psprintf("%s dbname=%s", conn_params, dopt->cparams.dbname);
	if(dopt->cparams.pghost)
		conn_params = psprintf("%s host=%s", conn_params, dopt->cparams.pghost);
	if(dopt->cparams.pgport)
		conn_params = psprintf("%s port=%s", conn_params, dopt->cparams.pgport);
	if(dopt->cparams.username)
		conn_params = psprintf("%s user=%s", conn_params, dopt->cparams.username);
	connection = PQconnectdb(conn_params);

	while (mask_column_info_cell != NULL)
	{
		MaskColumnInfo* cur_mask_column_info = (MaskColumnInfo*) mask_column_info_cell->ptr;
		func_name_buffer = pg_strdup(cur_mask_column_info->func);
		canonicalize_path(func_name_buffer);
		mask_func_file = fopen(func_name_buffer, "r");
		if (mask_func_file != NULL) /* then it is a file with function*/
		{
			addFuncToDatabase(cur_mask_column_info, mask_func_file, connection);
		}
		else /* function stored in DB*/
		{
			schema_name_buffer = strtok(cur_mask_column_info->func, ".");
			func_name_buffer = strtok(NULL, ".");
			if (func_name_buffer == NULL) /* found function without schemaname */
			{
				strcpy(cur_mask_column_info->schema, "public");
				strcpy(cur_mask_column_info->func, schema_name_buffer);
			}
			else
			{
				strcpy(cur_mask_column_info->schema, schema_name_buffer);
				strcpy(cur_mask_column_info->func, func_name_buffer);
			}
		}
		mask_column_info_cell = mask_column_info_cell->next;
	}

	PQfinish(connection);
	free(conn_params);
}

static void
addFuncToDatabase(MaskColumnInfo* cur_mask_column_info, FILE* mask_func_file, PGconn *connection)
{
	/*
	 * All buffers are the length of 64 because in PostgreSQL length of identifier
	 * be it name of column, table, etc are 63 chars long, + 64th for \0 
	 */

	PQExpBufferData query;
	char* common_buff = (char*) pg_malloc(64 * sizeof(char));
	char* func_name_buff;
	char* argument_type_buff = (char*) pg_malloc(64 * sizeof(char));
	char* func_language_buff = (char*) pg_malloc(64 * sizeof(char));
	char* func_body_buff = (char*) pg_malloc(sizeof(char));
	char* schema_name_buff = (char*) pg_malloc(64 * sizeof(char));

	func_body_buff[0] = 0;
	fgets(common_buff, 64, mask_func_file);
	func_name_buff = strdup(strtok(common_buff, " ,\n\t"));
	fgets(common_buff, 64, mask_func_file);
	argument_type_buff = strdup(strtok(common_buff, " .,\n\t"));
	fgets(common_buff, 64, mask_func_file);
	func_language_buff = strdup(strtok(common_buff, " ,\n\t"));
	free(common_buff);

	/*
	 * Body of a function can be big, so we choose 512 as buffer size.
	 */

	common_buff = (char*) pg_malloc(512 * sizeof(char)); 
	while(fgets(common_buff, 512, mask_func_file))
	{
		func_body_buff = psprintf("%s%s", func_body_buff, common_buff);
	}
	
	initPQExpBuffer(&query);
	appendPQExpBuffer(&query, "CREATE OR REPLACE FUNCTION %s (IN elem %s, OUT res %s) RETURNS %s AS $BODY$ \nBEGIN\n%s\nRETURN;\nEND\n$BODY$ LANGUAGE %s;",
	 						func_name_buff, argument_type_buff, argument_type_buff, argument_type_buff,
							func_body_buff, func_language_buff);

	PQexec(connection, query.data);

	schema_name_buff = strtok(pg_strdup(func_name_buff), ".");
	func_name_buff = strtok(NULL, ".");
	if (func_name_buff == NULL) /* found function without schemaname */
	{
		strcpy(cur_mask_column_info->schema, "public");
		strcpy(cur_mask_column_info->func, schema_name_buff);
	}
	else
	{
		strcpy(cur_mask_column_info->schema, schema_name_buff);
		strcpy(cur_mask_column_info->func, func_name_buff);
	}
}

/*
* maskColumns - modifies SELECT queries to mask columns that need masking
* last argument is only for INSERT case, not used in COPY case.
*/

static void
maskColumns(TableInfo *tbinfo, char* column_list, PQExpBuffer* q, SimpleStringList* column_names)
{
	char* copy_column_list = pg_strdup(column_list);
	char* current_column_name = strtok(copy_column_list, " ,()");
	char* masked_query = (char*)pg_malloc(sizeof(char));

	while (current_column_name != NULL)
	{
		SimplePtrListCell* mask_column_info_cell = mask_column_info_list.head;
		MaskColumnInfo* cur_mask_column_info = (MaskColumnInfo*) mask_column_info_cell->ptr;
		while (mask_column_info_cell != NULL &&
			  (strcmp(cur_mask_column_info->column, current_column_name) ||
				strcmp(cur_mask_column_info->table, tbinfo->dobj.name)))
			{
				if (!strcmp(cur_mask_column_info->table, "") &&
					!strcmp(cur_mask_column_info->column, current_column_name))
					break;

				mask_column_info_cell = mask_column_info_cell->next;
				if (mask_column_info_cell)
					cur_mask_column_info = (MaskColumnInfo*) mask_column_info_cell->ptr;
			}

		if (mask_column_info_cell != NULL)
		{
			/*current table name is stored in tbinfo->dobj.name*/
			if (!strcmp(cur_mask_column_info->table, "") ||
				!strcmp(cur_mask_column_info->table, tbinfo->dobj.name))
				masked_query = psprintf("%s.%s(%s)", cur_mask_column_info->schema,
										cur_mask_column_info->func, current_column_name);
			else
				masked_query = psprintf("%s", current_column_name);
		}
		else
			masked_query = psprintf("%s", current_column_name);

		if (column_names)
			simple_string_list_append(column_names, current_column_name);
		current_column_name = strtok(NULL, " ,()");
		if (current_column_name != NULL)
			masked_query = psprintf("%s, ", masked_query);
		appendPQExpBufferStr(*q, masked_query);
	}
	free(masked_query);
}

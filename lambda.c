//usr/bin/bash -c $(awk 'NR>=3' lambda.c)
#ifdef BOOTSTRAP
gcc -g -Wall -Wpedantic -Wextra lambda.c -o meta_lambda
if [ $? -eq 0 ]; then
    ./meta_lambda $1
else
    echo "error build failed"
    exit -1
fi

exit 0
#endif

/*
 * Simple metaprogram to handle name mangling. Does not support closures.
 * 'Brute force' program that takes the whole lambda expression moves it to the
 * top of the translation unit and replaces the original expression with the
 * the mangled function name. It is brute force in the sense that no type
 * checking is done and acts like an extension of the existing macro system
 *
 * Syntax:
 *     lambda(<return-type>, <arg-type-1> <arg-1>, ...,
 *     {
 *          <function-body>
 *     });
 *
 * Ex:
 *      #include <lambda.h>
 *
 *      bool (*lambda_expression)(int, int) = lambda(bool, int x, int y,
 *      {
 *          return x == y;
 *      });
 *
 * Becomes
 *      #include <lambda.h>
 *
 *      bool
 *      mangled_function_name(int x, int y)
 *      {
 *          return x == y;
 *      }
 *      ...
 *
 *      bool (*lambda_expression)(int, int) = mangled_function_name;
 *
 * Note:
 *      lambda expressions are inserted after the #include <lambda.h>
 */

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <ctype.h>

#include <stdint.h>
#include <stdbool.h>
typedef int64_t i64;
typedef uint64_t u64;
typedef int32_t i32;
typedef uint32_t u32;
typedef uint16_t u16;
typedef int16_t i16;
typedef float f32;
typedef double f64;
typedef uint8_t u8;

#define FILE_BUFFER_SIZE 4088
typedef struct
{
    long long size;
    const char *data;
} String;

typedef struct Lambda
{
    String return_type, function_args, function_body, raw_lambda;
    struct Lambda *next;
} Lambda;

typedef struct
{
    size_t total_size;
    size_t last_chunk_size;
    struct StringBuilderNode
    {
        char data[FILE_BUFFER_SIZE];
        struct StringBuilderNode *next;
    } *head, *tail;
} StringBuilder;

typedef struct StringBuilderNode StringBuilderNode;

static const char *
find_next_keyword_that_has_no_repeating_characters(const char *file_data, const char *keyword)
{
    assert(file_data);
    assert(keyword);

    int file_head, keyword_head;
    keyword_head = file_head = 0;

    while (file_data[file_head] && keyword[keyword_head])
    {
        if (file_data[file_head] != keyword[keyword_head])
        {
            keyword_head = 0;
        }
        else
        {
            keyword_head += 1;
        }
        file_head += 1;
    }

    // keyword[keyword_head] == '\0' => strlen(keyword) == keyword_head
    assert(keyword[keyword_head] || strlen(keyword) == keyword_head);
    return keyword[keyword_head] ? NULL : (file_data + file_head - keyword_head);
}

static const char *
find_next_non_whitespace_character(const char *str_in)
{
    while (*str_in)
    {
        if (!isspace(*str_in))
        {
            return str_in;
        }
        else
        {
            str_in++;
        }
    }
    return NULL;
}

static long long
find_next_matching_character(const char *str_in, char c)
{
    for (int i = 0; str_in[i]; i++)
    {
        if (str_in[i] == c)
        {
            return i;
        }
    }

    return -1;
}

static bool
parse_lambda(const char *lambda_start, Lambda *lambda_out)
{
    if (strncmp(lambda_start, "lambda", 6)) return false;
    lambda_out->raw_lambda.data = lambda_start;
    lambda_out->raw_lambda.size = 0;
    lambda_start = find_next_non_whitespace_character(lambda_start + 6);
    if (lambda_start)

    if (*lambda_start != '(') return false;
    int num_left_parentheses, num_right_parentheses;
    num_left_parentheses = num_right_parentheses = 0;

    int lambda_size = 0;
    while (true)
    {
        switch (lambda_start[lambda_size])
        {
            case '\0':
                return false;
            case '(':
                num_left_parentheses += 1;
                break;
            case ')':
                num_right_parentheses += 1;
                break;
        }
        if (num_left_parentheses == num_right_parentheses)
            break;
        lambda_size += 1;
    }

    assert(num_left_parentheses == num_right_parentheses);
    lambda_out->raw_lambda.size = lambda_size + 7;


    // Parse return type
    lambda_out->return_type.data = lambda_start + 1;
    lambda_out->return_type.size =
        find_next_matching_character(
            lambda_out->return_type.data,
            ','
        );
    if (lambda_out->return_type.size == -1 || lambda_out->return_type.size >= lambda_size - 1)
        return false;

    assert(*(lambda_out->return_type.data + lambda_out->return_type.size - 1) != ',');
    assert(*(lambda_out->return_type.data + lambda_out->return_type.size) == ',');


    // Parse function body
    long long function_args_size, function_body_start_offset, function_body_end_offset;
    function_args_size = function_body_start_offset = 0;
    function_body_start_offset =
        find_next_matching_character(
            lambda_out->return_type.data + 1,
            '{'
        );
    function_body_end_offset =
        find_next_matching_character(
            lambda_out->return_type.data + 1,
            '}'
        );

    if (function_body_start_offset >= function_body_end_offset)
        return false;

    lambda_out->function_body.data =
        lambda_out->return_type.data + function_body_start_offset + 1;
    lambda_out->function_body.size =
        function_body_end_offset - function_body_start_offset;

    const char *comma_after_function_body_end_ptr =
        lambda_out->function_body.data + lambda_out->function_body.size;

    long long comma_after_function_body_end_offset =
        find_next_matching_character(
            lambda_out->function_body.data + lambda_out->function_body.size,
            ','
        );

    if (comma_after_function_body_end_offset != -1)
    {
        comma_after_function_body_end_ptr += comma_after_function_body_end_offset;
        if (comma_after_function_body_end_ptr < lambda_out->function_body.data + lambda_out->function_body.size)
            return false;
    }

    lambda_out->function_body.size += 1;
    assert(*(lambda_out->function_body.data) == '{');
    assert(*(lambda_out->function_body.data + lambda_out->function_body.size - 1) == '}');


    // Parse function args
    lambda_out->function_args.data = lambda_out->return_type.data;
    lambda_out->function_args.data += lambda_out->return_type.size + 1;
    lambda_out->function_args.size = 0;

    while (true)
    {
        const char *function_args_end = lambda_out->function_args.data;
        function_args_end += lambda_out->function_args.size;

        long long next_comma_offset = find_next_matching_character(
            function_args_end,
            ','
        );

        if (next_comma_offset == -1)
            break;

        if (function_args_end + next_comma_offset + 1 >= lambda_out->function_body.data)
            break;

        lambda_out->function_args.size += next_comma_offset + 1;
    }

    lambda_out->function_args.size -= 1;
    assert(*(lambda_out->function_args.data - 1) == ',');
    assert(*lambda_out->function_args.data != ',');
    assert(*(lambda_out->function_args.data + lambda_out->function_args.size - 1) != ',');
    assert(*(lambda_out->function_args.data + lambda_out->function_args.size) == ',');
    return true;
}

static void
append_string(StringBuilder *string_builder, const char *src_str, size_t src_len)
{
    assert(src_len != 0);
    StringBuilderNode *last_chunk = string_builder->tail;
    if (!last_chunk)
    {
        last_chunk = malloc(sizeof(StringBuilderNode));
        last_chunk->next = NULL;

        string_builder->head = string_builder->tail = last_chunk;
        string_builder->last_chunk_size = 0;
    }

    if (FILE_BUFFER_SIZE - string_builder->last_chunk_size >= src_len)
    {
        memcpy(
            &last_chunk->data[string_builder->last_chunk_size],
            src_str,
            src_len
        );
        string_builder->last_chunk_size = string_builder->last_chunk_size + src_len;
    }
    else
    {
        memcpy(
            &last_chunk->data[string_builder->last_chunk_size],
            src_str,
            FILE_BUFFER_SIZE - string_builder->last_chunk_size
        );
        src_str += FILE_BUFFER_SIZE - string_builder->last_chunk_size;
        src_len -= FILE_BUFFER_SIZE - string_builder->last_chunk_size;

        while (src_len > FILE_BUFFER_SIZE)
        {
            assert(string_builder->head);
            assert(string_builder->tail);

            last_chunk = malloc(sizeof(StringBuilderNode));
            last_chunk->next = NULL;

            string_builder->tail->next = last_chunk;
            string_builder->tail = last_chunk;

            memcpy(last_chunk->data, src_str, FILE_BUFFER_SIZE);
            src_str += FILE_BUFFER_SIZE;
            src_len -= FILE_BUFFER_SIZE;
        }

        assert(string_builder->head);
        assert(string_builder->tail);

        last_chunk = malloc(sizeof(StringBuilderNode));
        last_chunk->next = NULL;

        string_builder->tail->next = last_chunk;
        string_builder->tail = last_chunk;

        memcpy(last_chunk->data, src_str, src_len);
        string_builder->last_chunk_size = src_len;
    }

    string_builder->total_size += src_len;
}

int
main(int argc, const char *argv[argc])
{
    String file;
    //assert(argc == 2 && "Bad args.");

    int fd = open("test.meta.c", O_RDWR);
    assert(fd > 2 && "Failed to open input file.");

    struct stat file_stats;
    assert(!fstat(fd, &file_stats) && "Failed to run stat on input file.");

    char *buffer = malloc(file_stats.st_size + 1);
    assert(read(fd, (char *) buffer, file_stats.st_size) == file_stats.st_size);

    buffer[file_stats.st_size] = '\0';
    file.data = buffer;

    size_t lambda_id = 0;
    const char *lambda_include_ptr =
        find_next_keyword_that_has_no_repeating_characters(
            file.data,
            "#include \"lambda.h\""
        );
    assert(lambda_include_ptr);

    const char *next_potential_lambda = lambda_include_ptr;
    const char *end_of_last_lambda_ptr = lambda_include_ptr;

    StringBuilder lambdas_text = { 0 };
    StringBuilder after_lambdas_text = { 0 };

    while (true)
    {
        next_potential_lambda = find_next_keyword_that_has_no_repeating_characters(
            next_potential_lambda,
            "lambda"
        );
        if (next_potential_lambda == NULL) break;

        Lambda lambda;
        if (parse_lambda(next_potential_lambda, &lambda))
        {
            // Print lambda
            append_string(&lambdas_text, "static ", 7);
            append_string(&lambdas_text, lambda.return_type.data, lambda.return_type.size);
            append_string(&lambdas_text, "\n", 1);

            char new_lambda_name[20] = { "lambda_" };

            int i = 7;
            int temp_lambda_id = lambda_id;
            while (temp_lambda_id)
            {
                new_lambda_name[i++] = temp_lambda_id % 10 + '0';
                temp_lambda_id /= 10;
            }

            if (i == 7)
            {
                new_lambda_name[7] = '0';
                i = 8;
            }
            append_string(&lambdas_text, new_lambda_name, i);
            append_string(&lambdas_text, "(", 1);
            append_string(&lambdas_text, lambda.function_args.data, lambda.function_args.size);
            append_string(&lambdas_text, ")\n", 2);

            append_string(&lambdas_text, lambda.function_body.data, lambda.function_body.size);
            append_string(&lambdas_text, "\n\n", 2);

            lambda_id += 1;


            // Print the rest
            assert(lambda.raw_lambda.data >= end_of_last_lambda_ptr);
            append_string(
                &after_lambdas_text,
                end_of_last_lambda_ptr,
                lambda.raw_lambda.data - end_of_last_lambda_ptr
            );
            append_string(&after_lambdas_text, new_lambda_name, i);
            end_of_last_lambda_ptr = lambda.raw_lambda.data + lambda.raw_lambda.size;
        }

        if (lambda.raw_lambda.size != 0)
        {
            next_potential_lambda += lambda.raw_lambda.size;
        }
        else
        {
            next_potential_lambda += 6;
        }
    }

    assert(end_of_last_lambda_ptr >= file.data);
    assert(file_stats.st_size >= end_of_last_lambda_ptr - file.data);
    append_string(
        &after_lambdas_text,
        end_of_last_lambda_ptr,
        file_stats.st_size - (end_of_last_lambda_ptr - file.data)
    );

    int outfile_fd =
        open("test.c", O_WRONLY | O_TRUNC | O_CREAT, file_stats.st_mode);

    write(outfile_fd, file.data, lambda_include_ptr - file.data);

    for (StringBuilderNode *node = lambdas_text.head; node; node = node->next)
    {
        if (node->next)
            write(outfile_fd, node->data, FILE_BUFFER_SIZE);
        else
            write(outfile_fd, node->data, lambdas_text.last_chunk_size);
    }

    for (StringBuilderNode *node = after_lambdas_text.head; node; node = node->next)
    {
        if (node->next)
            write(outfile_fd, node->data, FILE_BUFFER_SIZE);
        else
            write(outfile_fd, node->data, after_lambdas_text.last_chunk_size);
    }

    close(outfile_fd);
    free(buffer);
}

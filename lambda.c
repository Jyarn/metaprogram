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

static inline char
chunk_to_b64(unsigned char c)
{
    if (c < 10) return c + '0';
    else if (c < 36) return c - 10 + 'A';
    else if (c < 62) return c - 36 + 'a';
    else if (c == 62) return '_';
    else if (c == 63) return '$';
    assert(false);
}

static void
format_lambda(Lambda lambda, size_t function_id)
{
    char function_id_base64[11];
    function_id_base64[10] = chunk_to_b64(function_id & 0x3F);
    function_id_base64[9] = chunk_to_b64((function_id >>= 6) & 0x3F);
    function_id_base64[8] = chunk_to_b64((function_id >>= 6) & 0x3F);
    function_id_base64[7] = chunk_to_b64((function_id >>= 6) & 0x3F);
    function_id_base64[6] = chunk_to_b64((function_id >>= 6) & 0x3F);
    function_id_base64[5] = chunk_to_b64((function_id >>= 6) & 0x3F);
    function_id_base64[4] = chunk_to_b64((function_id >>= 6) & 0x3F);
    function_id_base64[3] = chunk_to_b64((function_id >>= 6) & 0x3F);
    function_id_base64[2] = chunk_to_b64((function_id >>= 6) & 0x3F);
    function_id_base64[1] = chunk_to_b64((function_id >>= 6) & 0x3F);
    function_id_base64[0] = chunk_to_b64((function_id >>= 6) & 0x3F);

    printf("static %.*s\n", lambda.return_type.size, lambda.return_type.data);
    printf("lambda%.11s(%.*s)\n", function_id_base64, lambda.function_args.size, lambda.function_args.data);
    printf("%.*s\n", lambda.function_body.size, lambda.function_body.data);
}

int
main(int argc, const char *argv[argc])
{
    String file;
    //assert(argc == 2 && "Bad args.");

    struct
    {
        Lambda *head, *tail;
    } lambdas;

    int fd = open("./test.txt", O_RDWR);
    assert(fd > 2 && "Failed to open input file.");

    struct stat file_stats;
    assert(!fstat(fd, &file_stats) && "Failed to run stat on input file.");

    char *buffer = malloc(file_stats.st_size + 1);
    assert(read(fd, (char *) buffer, file_stats.st_size) == file_stats.st_size);

    buffer[file_stats.st_size] = '\0';
    file.data = buffer;

    size_t lambda_id = 0;
    const char *next_potential_lambda = file.data;

    size_t old_lambda_total_size, new_lambda_total_size;
    old_lambda_total_size = new_lambda_total_size = 0;
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
            format_lambda(lambda, lambda_id++);

            old_lambda_total_size += lambda.raw_lambda.size;

            new_lambda_total_size += lambda.return_type.size;
            new_lambda_total_size += lambda.function_args.size;
            new_lambda_total_size += lambda.function_body.size;
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

    free(buffer);
}

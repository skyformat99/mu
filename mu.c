/*
 * Mu stand-alone interpreter
 */

#include "mu.h"
#include "var.h"
#include "num.h"
#include "str.h"
#include "tbl.h"
#include "fn.h"
#include "err.h"

#include <string.h>
#include <stdio.h>

#define PROMPT_A "\033[32m> \033[0m"
#define PROMPT_B "\033[32m. \033[0m"
#define OUTPUT_A ""

#define ERR_START "\033[31m"
#define ERR_END   "\033[0m"

#define BUFFER_SIZE MU_MAXLEN


static tbl_t *scope = 0;
static tbl_t *args = 0;

static bool do_interpret = false;
static bool do_stdin = false;
static bool do_default = true;


static void printvar(var_t v, eh_t *eh) {
    str_t *out;

    if (isstr(v)) {
        out = getstr(v);
    } else {
        out = var_repr(v, eh);
    }

    printf("%.*s", str_getlen(out), str_getdata(out));
}

static void printrepr(var_t v, eh_t *eh) {
    printvar(vstr(var_repr(v, eh)), eh);
}

static void printerr(tbl_t *err) {
    mu_try_begin (eh) {
        printf("%s", ERR_START);
        printvar(tbl_lookup(err, vcstr("type", eh)), eh);
        printf(" error: ");
        printvar(tbl_lookup(err, vcstr("reason", eh)), eh);
        printf("%s\n", ERR_END);
    } mu_on_err (err) {
        printf("%serror handling error%s\n", ERR_START, ERR_END);
    } mu_try_end;
}

static void printoutput(var_t v, eh_t *eh) {
    printf("%s", OUTPUT_A);
    printrepr(v, eh);
    printf("\n");
}

static len_t prompt(data_t *input) {
    printf("%s", PROMPT_A);
    len_t len = 0;

    while (1) {
        data_t c = getchar();
        input[len++] = c;
        
        if (c == '\n')
            return len;
    }
}


// TODO move this scope declaration somewhere else
static var_t b_add(tbl_t *args, eh_t *eh) {
    return vdouble(getdouble(tbl_lookup(args, vuint(0))) +
                   getdouble(tbl_lookup(args, vuint(1))));
}

static var_t b_sub(tbl_t *args, eh_t *eh) {
    return vdouble(getdouble(tbl_lookup(args, vuint(0))) -
                   getdouble(tbl_lookup(args, vuint(1))));
}

static var_t b_equals(tbl_t *args, eh_t *eh) {
    return var_equals(tbl_lookup(args, vuint(0)),
                      tbl_lookup(args, vuint(1))) ? vuint(1) : vnil;
}

static var_t b_repr(tbl_t *args, eh_t *eh) {
    return vstr(var_repr(tbl_lookup(args, vuint(0)), eh));
}

static var_t b_print(tbl_t *args, eh_t *eh) {
    tbl_for_begin (k, v, args) {
        printvar(v, eh);
    } tbl_for_end;

    printf("\n");
    return vnil;
}

static void genscope(eh_t *eh) {
    scope = tbl_create(0, eh);

    tbl_t *ops = tbl_create(0, eh);
    tbl_assign(ops, vcstr("+", eh), vbfn(b_add, eh), eh);
    tbl_assign(ops, vcstr("-", eh), vbfn(b_sub, eh), eh);
    tbl_assign(ops, vcstr("==", eh), vbfn(b_equals, eh), eh);
    tbl_assign(scope, vcstr("ops", eh), vtbl(ops), eh);
    tbl_assign(scope, vcstr("repr", eh), vbfn(b_repr, eh), eh);
    tbl_assign(scope, vcstr("print", eh), vbfn(b_print, eh), eh);
}

static int genargs(int i, int argc, const char **argv, eh_t *eh) {
    args = tbl_create(argc-i, eh);

    for (; i < argc; i++) {
        tbl_append(args, vcstr(argv[i], eh), eh);
    }

    return i;
}


static void execute(const char *input, eh_t *eh) {
    fn_t *f = fn_create(0, vcstr(input, eh), eh);

    fn_call_in(f, 0, scope, eh);
}

static void load_file(FILE *file, eh_t *eh) {
    data_t *buffer = mu_alloc(BUFFER_SIZE, eh);
    size_t off = 0;

    size_t len = fread(buffer, 1, BUFFER_SIZE, file);

    if (ferror(file)) {
        mu_cerr(vcstr("io", eh), vcstr("encountered file reading error", eh), eh);
    }

    if (!memcmp(buffer, "#!", 2)) {
        for (off = 2; buffer[off] != '\n'; off++)
            ;

        off++;
    }

    var_t code = vnstr(buffer+off, len-off, eh);
    fn_t *f = fn_create(0, code, eh);

    fn_call_in(f, 0, scope, eh);
}

static void load(const char *name, eh_t *eh) {
    FILE *file;

    if (!(file = fopen(name, "r"))) {
        mu_cerr(vcstr("io", eh), vcstr("could not open file", eh), eh);
    }

    load_file(file, eh);

    fclose(file);
}

static mu_noreturn int interpret(eh_t *eh) {
    data_t *buffer = mu_alloc(BUFFER_SIZE, eh);

    while (1) {
        len_t len = prompt(buffer);
        var_t code = vnstr(buffer, len, eh);
        
        mu_try_begin (eh) {
            fn_t *f;

            mu_try_begin (eh) {
                f = fn_create_expr(0, code, eh);
            } mu_on_err (err) {
                f = fn_create(0, code, eh);
            } mu_try_end;

            var_t output = fn_call_in(f, 0, scope, eh);

            if (!isnil(output))
                printoutput(output, eh);

            var_dec(output);
            fn_dec(f);
        } mu_on_err (err) {
            printerr(err);
        } mu_try_end;
    }
}

static int run(eh_t *eh) {
    var_t mainfn = tbl_lookup(scope, vcstr("main", eh));

    if (isnil(mainfn))
        return 0;

    var_t code = var_call(mainfn, args, eh);

    if (isnil(code) || isnum(code))
        return (int)getnum(code);
    else
        return -1;
}


static void usage(const char *name) {
    printf("\n"
           "usage: %s [options] [program] [args]\n"
           "options:\n"
           "  -e string     execute provided string before program\n"
           "  -l file       import and execute file before program\n"
           "  -i            run interactively after program\n"
           "  --            stop handling options\n"
           "program: file to execute and run or '-' for stdin\n"
           "args: arguments passed to running program\n"
           "\n", name);
}

static int options(int i, int argc, const char **argv, eh_t *eh) {
    while (i < argc) {
        uint_t len = strlen(argv[i]);

        if (argv[i][0] != '-') {
            return i;
        } else if (len > 2) {
            return -1;
        }

        switch (argv[i++][1]) {
            case 'e':
                if (i >= argc)
                    return -1;
                execute(argv[i++], eh);
                do_default = false;
                break;

            case 'l':
                if (i >= argc)
                    return -1;
                load(argv[i++], eh);
                break;

            case 'i':
                do_interpret = true;
                break;

            case '\0':
                do_stdin = true;
                return i;

            case '-':
                return i;

            default:
                return -1;
        }
    }

    return i;
}


int main(int argc, const char **argv) {
    mu_try_begin (eh) {
        int i = 1;

        genscope(eh);

        if ((i = options(i, argc, argv, eh)) < 0) {
            usage(argv[0]);
            return -2;
        }

        if (i >= argc && !do_stdin) {
            if (!do_default && !do_interpret)
                return 0;
            else
                do_interpret = true;
        }

        if (i < argc || do_stdin) {
            if (do_stdin)
                load_file(stdin, eh);
            else
                load(argv[i++], eh);
        } else if (do_default) {
            do_interpret = true;
        } else if (!do_interpret) {
            return 0;
        }

        genargs(i, argc, argv, eh);

        if (do_interpret)
            return interpret(eh);
        else
            return run(eh);

    } mu_on_err (err) {
        printerr(err);

        // TODO fix this with constant allocations
        var_t code = tbl_lookup(err, vcstr("code", 0));
        if (isnum(code))
            return (int)getnum(code);
        else
            return -1;
    } mu_try_end;
}

# Mockoto - a simple C mock generator with Racket bindings

Mockoto is a tool help the testing of C code with [Racket](https://racket-lang.org).

Mockoto generates simple mock functions for C prototypes.
These mock functions do not do anything by default, but can be ajusted conforming the desired test case.
Mockoto also generates Racket bindings to all related types and hook functions.
In this way, one can write test cases and mock code in Racket for C code.


## Building

Ensure your system have these packages installed:
- cmake
- clang
- libclang-dev
- xxd

That should be sufficient to compile Mockoto, but if you want to write test cases in Racket you, of course, also need Racket installed.

To build Mockoto run:

	cd mockoto
	cmake -S . -B build
	make -j

You should now have the executable `build/mockoto`, which can be installed in your `PATH`.

## Usage

    mockoto --mode C header.h another.h > mock_code.c
    mockoto --mode H header.h another.h > mock_code.h
    mockoto --mode rkt header.h another.h > bindings.rkt

Your test case can be compiled with `mock_code.c`.
If you want to adapt the mock code from C, you can include `mock_code.h` and use the respective functions.

For a function `foo` declared in the given header file, the following functions are available:

    // This is the mock foo function, which does nothing by default.
    // The return value is a 0-ed value of the return type.
    retType foo(paramType ...);

    // Returns the number of times foo was called.
    // The counter resets once this function is called.
    int mockoto_foo_called(void);

    // Sets a different return value for foo in case foo has a return type
    void mockoto_foo_returns(retType r);

    // Sets a callback with the same signature as foo.
    // Whenever foo is called, the callback will be called and its return value will be used
    // instead of the default value of the mock.
    void mockoto_foo_hook(mockoto_foo_f cb);


With `bindings.rkt` you have access to the mock from Racket. Here is the `foo` example:

    #lang racket
    (require rackunit
             "bindings.rkt")
    (load-lib "libmock_code.so")
    (mock foo
      (lambda (param0 param1 ...)
        ;; do something
        ;; return the desired value
        some-value))
    (check-equal? (call foo ...) some-value)

See [example](example) for a running example.

## Mockoto Limitations

- no support for varargs
- assumes enums and structs have unique names


Vendored libraries for the tests

These build only with the tests and are never linked into a libchiaking library or the installed package.

- [GoogleTest](https://github.com/google/googletest) 1.17.0 vendored from its upstream release tag `v1.17.0` under [BSD-3-Clause](googletest/LICENSE); only `googletest/include` and `googletest/src` are kept, without GoogleMock. `tests/googletest.cmake` uses it when no installed GoogleTest is found

Vendored libraries for the core

- [sml](https://github.com/boost-ext/sml) 1.2.0 vendored under [BSL](https://github.com/boost-ext/sml/blob/master/LICENSE.md)
- [simdutf](https://github.com/simdutf/simdutf) 9.2.1 vendored as a full amalgamation under [Apache-2.0](https://github.com/simdutf/simdutf/blob/master/LICENSE-APACHE) or [MIT](https://github.com/simdutf/simdutf/blob/master/LICENSE-MIT)
- [Glaze](https://github.com/stephenberry/glaze) 9.0.0 vendored under [MIT](https://github.com/stephenberry/glaze/blob/main/LICENSE)
- [magic_enum](https://github.com/Neargye/magic_enum) 0.9.8 vendored under [MIT](https://github.com/Neargye/magic_enum/blob/master/LICENSE)
- [SPSCQueue](https://github.com/rigtorp/SPSCQueue) 1.1 (`include/rigtorp/SPSCQueue.h` at tag v1.1) vendored unchanged under [MIT](rigtorp/LICENSE)
- [picohttpparser](https://github.com/h2o/picohttpparser) at commit 465a7ff09fbd3432fe56c673451f5460154d1f07 vendored under [MIT](https://github.com/h2o/picohttpparser/blob/master/picohttpparser.h)
- [nanopb](https://github.com/nanopb/nanopb) 0.4.9.2 runtime vendored under [zlib](https://github.com/nanopb/nanopb/blob/master/LICENSE.txt)
- [gf-complete](http://jerasure.org/jerasure/gf-complete) vendored under [BSD-3-Clause](gf-complete/License.txt) at commit fa54a4670a5705c84abf6c24b92b0cd479625478 of the fork chiaki-ng uses (https://github.com/streetpea/gf-complete, from https://git.sr.ht/~thestr4ng3r/gf-complete), which removes upstream's `bzero` calls and `getopt` includes; only the GF(2^w) core and its NEON sources are kept, built by `cmake/Vendored.cmake` without autotools, with NEON on for AArch64 targets
- [Jerasure](http://jerasure.org/jerasure/jerasure) 2.0 vendored under [BSD-3-Clause](jerasure/License.txt) at commit 505ccb4bc69eee8d7ea40a6089a056b99671134f of the fork chiaki-ng uses (https://github.com/streetpea/jerasure, from https://git.sr.ht/~thestr4ng3r/jerasure), which replaces upstream's `bzero` calls with `memset`; only `jerasure.c`, `cauchy.c` and `galois.c` with their headers are kept
- [micro-ecc](https://github.com/kmackay/micro-ecc) vendored under [BSD-2-Clause](https://github.com/kmackay/micro-ecc/blob/master/LICENSE.txt) as carried by the chiaki-ng Switch fork's libnx crypto backend, which adds secp521r1 and a separate secp521r1-only instantiation in `uECC_p521.c`
- [MPMCQueue](https://github.com/rigtorp/MPMCQueue) at commit b9808ede08f26fa9df4df4e081d19cace8f6c6ea (`include/rigtorp/MPMCQueue.h`) vendored under [MIT](rigtorp/LICENSE), with the C++23-deprecated `std::aligned_storage` slot replaced by an aligned byte array

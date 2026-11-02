#!/usr/bin/env python3
import argparse
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]

CURL_OPTIONS = [
    "-DBUILD_SHARED_LIBS=OFF",
    "-DBUILD_STATIC_LIBS=ON",
    "-DBUILD_CURL_EXE=OFF",
    "-DBUILD_TESTING=OFF",
    "-DBUILD_LIBCURL_DOCS=OFF",
    "-DBUILD_MISC_DOCS=OFF",
    "-DENABLE_CURL_MANUAL=OFF",
    "-DCURL_DISABLE_SOCKETPAIR=ON",
    "-DENABLE_THREADED_RESOLVER=OFF",
    "-DENABLE_ARES=OFF",
    "-DUSE_NGHTTP2=OFF",
    "-DENABLE_IPV6=OFF",
    "-DENABLE_UNIX_SOCKETS=OFF",
    "-DCURL_DISABLE_LDAP=ON",
    "-DCURL_DISABLE_LDAPS=ON",
    "-DCURL_USE_LIBPSL=OFF",
    "-DCURL_USE_LIBSSH2=OFF",
    "-DUSE_LIBIDN2=OFF",
    "-DCURL_BROTLI=OFF",
    "-DCURL_ZSTD=OFF",
    "-DCURL_USE_OPENSSL=ON",
]

TESTS = [
    "platform_http_curl_tests",
    "platform_http_curl_multi_tests",
    "platform_websocket_curl_tests",
]


def run(command, env=None):
    print("+", " ".join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], check=True, env=env)


def source_tree(source, work):
    if source.is_dir():
        return source
    target = work / "curl-source"
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    with tarfile.open(source) as archive:
        archive.extractall(target, filter="data")
    entries = [entry for entry in target.iterdir() if entry.is_dir()]
    if len(entries) != 1:
        sys.exit(f"expected one directory in {source}")
    return entries[0]


def build_curl(source, work, openssl, jobs):
    build = work / "curl-build"
    prefix = work / "curl-prefix"
    options = list(CURL_OPTIONS)
    if openssl:
        options.append(f"-DOPENSSL_ROOT_DIR={openssl}")
    run(["cmake", "-S", source, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_INSTALL_PREFIX={prefix}", *options])
    run(["cmake", "--build", build, "--parallel", jobs])
    run(["cmake", "--install", build])
    return prefix


def build_core(prefix, work, openssl, jobs):
    build = work / "core-build"
    search = [str(prefix)] + ([str(openssl)] if openssl else [])
    run(["cmake", "-S", REPOSITORY, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug",
        "-DBUILD_TESTING=ON", f"-DCURL_ROOT={prefix}", f"-DCMAKE_PREFIX_PATH={';'.join(search)}",
        "-DCMAKE_FIND_PACKAGE_PREFER_CONFIG=ON"])
    run(["cmake", "--build", build, "--parallel", jobs, "--target", *TESTS])
    return build


def run_tests(build):
    env = dict(os.environ, LIBCHIAKING_EXPECT_CONSTRAINED_CURL="1")
    run(["ctest", "--test-dir", build, "--output-on-failure", "-R", "^(" + "|".join(TESTS) + ")$"], env=env)


def main():
    parser = argparse.ArgumentParser(
        description="Build a minimal libcurl without socketpair wakeup, threaded DNS, HTTP/2 or IPv6, as consoles "
        "such as the Nintendo Switch ship, and run the curl provider tests against it.")
    parser.add_argument("--curl-source", type=Path, required=True,
        help="a curl release tarball or an unpacked curl source directory")
    parser.add_argument("--work", type=Path, default=REPOSITORY / "build-constrained-curl")
    parser.add_argument("--openssl", type=Path, default=os.environ.get("OPENSSL_ROOT_DIR"))
    parser.add_argument("--jobs", default=str(os.cpu_count() or 4))
    arguments = parser.parse_args()
    work = arguments.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    source = source_tree(arguments.curl_source.resolve(), work)
    prefix = build_curl(source, work, arguments.openssl, arguments.jobs)
    build = build_core(prefix, work, arguments.openssl, arguments.jobs)
    run_tests(build)


if __name__ == "__main__":
    main()

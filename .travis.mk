#
# Travis CI rules
#

# LuaJIT integration testing

cmake_build:
	${ARCH} cmake . -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} ${CMAKE_EXTRA_PARAMS}
	${ARCH} cmake --build . --parallel ${NPROC}

cmake_luajit_test:
	${ARCH} cmake --build . --parallel ${NPROC} --target LuaJIT-test

test_run_tarantool_test:
	cd test && ${ARCH} ./test-run.py --vardir ${VARDIR} --force $(TEST_RUN_EXTRA_PARAMS)

tarantool_linux_deps:
	sudo apt update -y && sudo apt -y install git build-essential cmake \
		make ninja-build zlib1g-dev libreadline-dev libncurses5-dev \
		libssl-dev libunwind-dev libicu-dev \
		python3 python3-six python3-gevent python3-pip

tarantool_linux_tests: tarantool_linux_deps cmake_build cmake_luajit_test test_run_tarantool_test

# XXX: No arch need to be set for Linux runners.
luajit_Linux_aarch64_test: ARCH=
luajit_Linux_aarch64_test: NPROC=$$(nproc)
luajit_Linux_aarch64_test: tarantool_linux_tests
# XXX: No arch need to be set for Linux runners.
luajit_Linux_x86_64_test: ARCH=
luajit_Linux_x86_64_test: NPROC=$$(nproc)
luajit_Linux_x86_64_test: tarantool_linux_tests

tarantool_darwin_deps:
	${ARCH} brew install --force openssl readline curl icu4c libiconv zlib \
		cmake python@3.8 \
	|| ${ARCH} brew upgrade openssl readline curl icu4c libiconv zlib \
		cmake python@3.8
	${ARCH} pip3 install --force-reinstall -r test-run/requirements.txt

tarantool_darwin_prebuild:
	${ARCH} sysctl vm.swapusage

tarantool_darwin_pretest:
	ulimit -n ${MAX_FILES} || : && ulimit -n ;
	ulimit -u ${MAX_PROC}  || : && ulimit -u ;
	rm -rf ${VARDIR}

# TODO: add tarantool_darwin_posttest rule to return default
# settings back (e.g. when running tests via .travis.mk on the
# local machine).

# XXX: Tarantool tests are skipped for M1 until #6068 is resolved.
tarantool_darwin_M1_tests: tarantool_darwin_deps tarantool_darwin_prebuild cmake_build cmake_luajit_test
tarantool_darwin_tests: tarantool_darwin_deps tarantool_darwin_prebuild cmake_build cmake_luajit_test tarantool_darwin_pretest test_run_tarantool_test

# XXX: No arch need to be set for x86_64 runners.
luajit_Darwin_x86_64_test: ARCH=
luajit_Darwin_x86_64_test: NPROC=$$(sysctl -n hw.ncpu)
luajit_Darwin_x86_64_test: tarantool_darwin_tests
# XXX: GitHub agent is run via Rosetta on M1 hosts, hence we need
# to explicitly set ARM64 arch for all subcommands.
luajit_Darwin_Rosetta_test: ARCH=arch -arm64
luajit_Darwin_Rosetta_test: NPROC=$$(sysctl -n hw.ncpu)
luajit_Darwin_Rosetta_test: tarantool_darwin_M1_tests

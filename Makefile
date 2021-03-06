.PHONY: all
all: test

INSTALL_PREFIX ?= ${HOME}/.local/helpers
BUILD_PROXY_IO ?= ON
WITH_COVERAGE  ?= OFF

# Build with Ceph storge helper by default
WITH_CEPH    		?= ON
# Build with Swift storage helper by default
WITH_SWIFT   		?= ON
# Build with S3 storage helper by default
WITH_S3      		?= ON
# Build with GlusterFS storage helper by default
WITH_GLUSTERFS		?= ON
# Build with WebDAV storage helper by default
WITH_WEBDAV 		?= ON


%/CMakeCache.txt: **/CMakeLists.txt test/integration/* test/integration/**/*
	mkdir -p $*
	cd $* && cmake -GNinja -DCMAKE_BUILD_TYPE=$* \
	                       -DCODE_COVERAGE=${WITH_COVERAGE} \
	                       -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX} \
	                       -DBUILD_PROXY_IO=${BUILD_PROXY_IO} \
	                       -DWITH_CEPH=${WITH_CEPH} \
	                       -DWITH_SWIFT=${WITH_SWIFT} \
	                       -DWITH_S3=${WITH_S3} \
	                       -DWITH_GLUSTERFS=${WITH_GLUSTERFS} \
	                       -DWITH_WEBDAV=${WITH_WEBDAV} \
	                       -DOPENSSL_ROOT_DIR=${OPENSSL_ROOT_DIR} \
	                       -DOPENSSL_LIBRARIES=${OPENSSL_LIBRARIES} ..
	touch $@

##
## Submodules
##

submodules:
	git submodule sync --recursive ${submodule}
	git submodule update --init --recursive ${submodule}


.PHONY: release
release: release/CMakeCache.txt
	cmake --build release --target helpersStatic
	cmake --build release --target helpersShared

.PHONY: test-release
test-release: release/CMakeCache.txt
	cmake --build release

.PHONY: debug
debug: debug/CMakeCache.txt
	cmake --build debug --target helpersStatic
	cmake --build debug --target helpersShared

.PHONY: test
test: debug
	cmake --build debug
	cmake --build debug --target test

.PHONY: cunit
cunit: debug
	cmake --build debug
	cmake --build debug --target cunit

.PHONY: install
install: release
	cmake --build release --target install

.PHONY: coverage
coverage:
	lcov --directory `pwd`/debug --capture --output-file `pwd`/helpers.info
	lcov --remove `pwd`/helpers.info 'test/*' '/usr/*' 'asio/*' '**/messages/*' \
	                           'relwithdebinfo/*' 'debug/*' 'release/*' \
	                           'erlang-tls/*' \
														 --output-file `pwd`/helpers.info.cleaned
	genhtml -o `pwd`/coverage `pwd`/helpers.info.cleaned
	echo "Coverage written to `pwd`/coverage/index.html"

.PHONY: clean
clean:
	rm -rf debug release

.PHONY: clang-tidy
clang-tidy:
	cmake --build debug --target clang-tidy

.PHONY: clang-format
clang-format:
	docker run --rm -e CHOWNUID=${UID} -v ${PWD}:/root/sources onedata/clang-format-check:1.1

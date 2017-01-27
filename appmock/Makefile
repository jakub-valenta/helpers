.PHONY: deps

BASE_DIR         = $(shell pwd)
GIT_URL := $(shell git config --get remote.origin.url | sed -e 's/\(\/[^/]*\)$$//g')
GIT_URL := $(shell if [ "${GIT_URL}" = "file:/" ]; then echo 'ssh://git@git.plgrid.pl:7999/vfs'; else echo ${GIT_URL}; fi)
ONEDATA_GIT_URL := $(shell if [ "${ONEDATA_GIT_URL}" = "" ]; then echo ${GIT_URL}; else echo ${ONEDATA_GIT_URL}; fi)
export ONEDATA_GIT_URL

all: rel

upgrade:
	./rebar3 upgrade

compile:
	#
	# Replace local git repo references in rebar.lock with GitHub
	# to enable builds without accessing private repository
	#
	@ if ! [[ x"${ONEDATA_GIT_URL}" == "x" ]] && [[ -f ./rebar.lock ]]; then \
		sed -i.bak 's/ssh:\/\/git@git\.plgrid\.pl:7999\/vfs/https:\/\/github.com\/onedata/g' rebar.lock; \
	fi
	./rebar3 compile

rel: compile
	./rebar3 release

start:
	_build/default/rel/appmock/bin/appmock console

clean:
	./rebar3 clean

distclean: clean
	./rebar3 clean --all

##
## Dialyzer targets local
##

# Dialyzes the project.
dialyzer:
	./rebar3 dialyzer
CFLAGS=-Wall -Werror -g3
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')
cc=gcc -std=gnu99
libcc=gcc
VERSION=0.1.0
SOVERSION=0
BUILD_DIR=$(shell pwd)/build/
LIB_DIR=$(BUILD_DIR)lib/
BIN_DIR=$(BUILD_DIR)bin/
PREFIX?=/usr/local/
INSTALL_LIB=$(PREFIX)/lib/
INSTALL_BIN=$(PREFIX)/bin/
ERLFLAGS=-smp -W1 -Werror -b beam -I./include -o $(BIN_DIR)
ERL_DIR=$(shell echo 'io:format("~s~n",[code:root_dir()]),init:stop().' | erl | sed -n '/^1>/s/^1> //p')
ERLI_DIR=$(shell echo 'io:format("~s~n",[code:lib_dir(erl_interface)]),init:stop().' | erl | sed -n '/^1>/s/^1> //p')
ERLINCLUDES=-I$(ERL_DIR)/usr/include/ -I$(ERLI_DIR)/include/
ERLLIBS=-L$(ERL_DIR)/usr/lib/ -L$(ERLI_DIR)/lib/
INCLUDES=-I./include $(ERLINCLUDES)

MATH_LINKER=
ifeq ($(uname_S),Darwin)
	# Do nothing
else
	MATH_LINKER=-lm
endif

all: liboleg server

liboleg:
	$(cc) $(CFLAGS) $(INCLUDES) -c -fPIC ./src/murmur3.c
	$(cc) $(CFLAGS) $(INCLUDES) -c -fPIC ./src/oleg.c
	$(cc) $(CFLAGS) $(INCLUDES) -c -fPIC ./src/dump.c
	$(cc) $(CFLAGS) $(INCLUDES) -c -fPIC ./src/logging.c
	$(cc) $(CFLAGS) $(INCLUDES) -c -fPIC ./src/aol.c
	$(cc) $(CFLAGS) $(INCLUDES) -c -fpic ./src/port_driver.c
	$(libcc) $(CFLAGS) $(INCLUDES) -o $(LIB_DIR)liboleg.so murmur3.o logging.o dump.o aol.o oleg.o -fpic -shared $(MATH_LINKER)
	$(libcc) $(CFLAGS) $(INCLUDES) $(ERLLIBS) -L$(LIB_DIR) -o $(LIB_DIR)libolegserver.so port_driver.o -fpic -shared $(MATH_LINKER) -loleg -lei
	$(cc) $(CFLAGS) $(INCLUDES) -c ./src/test.c
	$(cc) $(CFLAGS) $(INCLUDES) -c ./src/main.c
	$(libcc) $(CFLAGS) $(INCLUDES) -L$(LIB_DIR) -o $(BIN_DIR)oleg_test test.o main.o $(MATH_LINKER) -loleg

server:
	erlc $(ERLFLAGS) ./src/ol_database.erl
	erlc $(ERLFLAGS) ./src/ol_http.erl
	erlc $(ERLFLAGS) ./src/ol_parse.erl
	erlc $(ERLFLAGS) ./src/olegdb.erl

install:
	@mkdir -p $(INSTALL_LIB)
	@mkdir -p $(INSTALL_BIN)
	install $(LIB_DIR)liboleg.so $(INSTALL_LIB)liboleg.so.$(VERSION)
	ln -fs $(INSTALL_LIB)liboleg.so.$(VERSION) $(INSTALL_LIB)liboleg.so
	ln -fs $(INSTALL_LIB)liboleg.so.$(VERSION) $(INSTALL_LIB)liboleg.so.$(SOVERSION)
	install $(LIB_DIR)libolegserver.so $(INSTALL_LIB)libolegserver.so.$(VERSION)
	ln -fs $(INSTALL_LIB)libolegserver.so.$(VERSION) $(INSTALL_LIB)libolegserver.so
	ln -fs $(INSTALL_LIB)libolegserver.so.$(VERSION) $(INSTALL_LIB)libolegserver.so.$(SOVERSION)

test:
	./run_tests.sh

clean:
	rm $(BIN_DIR)*
	rm $(LIB_DIR)*
	rm *.o

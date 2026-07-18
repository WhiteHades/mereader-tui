.POSIX:

CC = cc
PKGS = ncursesw sqlite3 libxml-2.0 libzip libpcre2-8 gdk-pixbuf-2.0 poppler-glib cairo
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Iinclude $(shell pkg-config --cflags $(PKGS))
CFLAGS ?= -O2 -g
CFLAGS += -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 \
	-Wstrict-prototypes -Wmissing-prototypes -Werror=implicit-function-declaration
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lm -pthread

SOURCES = \
	src/app.c \
	src/common.c \
	src/config.c \
	src/database.c \
	src/document.c \
	src/epub.c \
	src/html.c \
	src/layout.c \
	src/mobi.c \
	src/platform.c \
	src/tui.c \
	src/main.c
OBJECTS = $(SOURCES:src/%.c=build/%.o)
TEST_SOURCES = tests/test_main.c tests/test_common.c tests/test_config.c tests/test_database.c \
	tests/test_document.c tests/test_layout.c
TEST_OBJECTS = $(TEST_SOURCES:tests/%.c=build/tests/%.o)
LIB_OBJECTS = $(filter-out build/main.o,$(OBJECTS))

.PHONY: all clean format install sanitize test tests python-tests

all: build/baca

build/baca: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

build/tests/test_baca: $(LIB_OBJECTS) $(TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(LIB_OBJECTS) $(TEST_OBJECTS) $(LDLIBS)

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

build/tests/%.o: tests/%.c
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

test tests: build/tests/test_baca build/baca
	./build/tests/test_baca
	./tests/cli_test.sh ./build/baca

python-tests:
	poetry run python -m pytest tests/test_html_parser.py

sanitize: CFLAGS = -O1 -g -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Werror=implicit-function-declaration \
	-fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: LDFLAGS += -fsanitize=address,undefined
sanitize: clean test

format:
	clang-format -i include/baca/*.h src/*.c tests/*.c

install: build/baca
	install -Dm755 build/baca "$(DESTDIR)$(PREFIX)/bin/baca"
	install -Dm644 resources/config.ini "$(DESTDIR)$(PREFIX)/share/baca/config.ini"

clean:
	rm -rf build

-include $(OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)

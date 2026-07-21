.POSIX:

CC = cc
PKGS = ncursesw sqlite3 glib-2.0 libxml-2.0 libzip libarchive libpcre2-8 gdk-pixbuf-2.0 poppler-glib cairo
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Iinclude $(shell pkg-config --cflags $(PKGS))
CFLAGS ?= -O2 -g
CFLAGS += -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 \
	-Wstrict-prototypes -Wmissing-prototypes -Werror=implicit-function-declaration
LDLIBS += $(shell pkg-config --libs $(PKGS)) -lm -pthread
TEST_LDLIBS = -lutil

SOURCES = \
	src/app.c \
	src/common.c \
	src/comic.c \
	src/config.c \
	src/database.c \
	src/document.c \
	src/epub.c \
	src/fb2.c \
	src/graphics.c \
	src/html.c \
	src/image.c \
	src/layout.c \
	src/library.c \
	src/mobi.c \
	src/pdf.c \
	src/platform.c \
	src/text.c \
	src/tui.c \
	src/main.c
OBJECTS = $(SOURCES:src/%.c=build/%.o)
TEST_SOURCES = tests/test_main.c tests/test_common.c tests/test_comic.c tests/test_config.c tests/test_database.c \
	tests/test_document.c tests/test_fb2.c tests/test_graphics.c tests/test_layout.c tests/test_library.c \
	tests/test_support.c tests/test_text.c

TEST_SOURCES += tests/test_pdf.c
TEST_OBJECTS = $(TEST_SOURCES:tests/%.c=build/tests/%.o)
LIB_OBJECTS = $(filter-out build/main.o,$(OBJECTS))

.PHONY: all clean format install sanitize test test-clang test-gcc tests

all: build/baca

build/baca: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

build/tests/test_baca: build/baca $(LIB_OBJECTS) $(TEST_OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(LIB_OBJECTS) $(TEST_OBJECTS) $(LDLIBS) $(TEST_LDLIBS)

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

build/tests/%.o: tests/%.c
	@mkdir -p build/tests
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

test tests: build/tests/test_baca build/baca
	./build/tests/test_baca
	sh ./tests/cli_test.sh ./build/baca

sanitize:
	$(MAKE) clean
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	$(MAKE) CC="$(CC)" \
		CFLAGS="-O1 -g -std=c23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
		-Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Werror=implicit-function-declaration \
		-fsanitize=address,undefined -fno-omit-frame-pointer" \
		LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined" test

test-gcc:
	$(MAKE) clean
	$(MAKE) CC=gcc test

test-clang:
	$(MAKE) clean
	$(MAKE) CC=clang test

format:
	clang-format -i include/baca/*.h src/*.c tests/*.c tests/*.h

install: build/baca
	install -Dm755 build/baca "$(DESTDIR)$(PREFIX)/bin/baca"
	install -Dm644 resources/config.ini "$(DESTDIR)$(PREFIX)/share/baca/config.ini"

clean:
	rm -rf build

-include $(OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)

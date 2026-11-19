# ADS HMI — service C
#
#   make            compile build/hmi-service
#   make test       tests unitaires (sanitizers) puis tests de bout en bout (Node >= 22)
#   make run        lance le service sur http://127.0.0.1:8080
#   make dist       kit de déploiement dans dist/ (voir deploy/)
#   make clean
#
# Windows :
#   sous Windows (MSYS2 UCRT64 ou WinLibs + mingw32-make) : mêmes commandes, détection automatique
#   make windows        compilation croisée depuis macOS/Linux : build/win/hmi-service.exe (gcc MinGW-w64)
#   make dist-windows   kit Windows (zip) à partir de build/win/hmi-service.exe

CFLAGS  ?= -O2 -g
WARN    := -std=c99 -pedantic -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wno-unused-parameter
DEFS    := -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
MINGW   ?= x86_64-w64-mingw32-gcc

# Windows natif (cmd, PowerShell ou MSYS2) : gcc MinGW, .exe, Winsock ; exécutable autonome (pas de DLL gcc)
ifeq ($(OS),Windows_NT)
CC      := gcc
EXE     ?= .exe
LDLIBS  ?= -lws2_32 -static
HOST    := Windows
else
CC      ?= cc
EXE     ?=
LDLIBS  ?= -lpthread -lm
HOST    := $(shell uname -s)
endif

SRC     := src/util.c src/plat.c src/types.c src/codec.c src/calc.c src/hub.c src/sim.c src/ads.c src/store.c src/http.c
CJSON   := third_party/cjson/cJSON.c
BIN     := build/hmi-service$(EXE)
TESTBIN := build/test_core$(EXE)
# AddressSanitizer se bloque à l'initialisation sur macOS récent : UBSan seul sous macOS
# (les fuites y sont contrôlées par l'outil « leaks »), ASan + UBSan ailleurs
ifeq ($(HOST),Darwin)
SAN     := -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
LEAKS   := leaks --atExit --
else ifeq ($(HOST),Windows)
# pas de sanitizers avec MinGW : tests compilés normalement
SAN     :=
LEAKS   :=
else
SAN     := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
LEAKS   :=
endif

.PHONY: all test unit e2e run dist windows dist-windows clean

all: $(BIN)

$(BIN): $(SRC) src/main.c $(CJSON) src/*.h | build
	$(CC) $(CFLAGS) $(WARN) $(DEFS) -o $@ $(SRC) src/main.c $(CJSON) $(LDLIBS)

# binaire de test instrumenté (détection des débordements, fuites, comportements indéfinis)
$(TESTBIN): $(SRC) tests/test_core.c $(CJSON) src/*.h | build
	$(CC) -O1 -g $(SAN) $(WARN) $(DEFS) -Isrc -o $@ $(SRC) tests/test_core.c $(CJSON) $(LDLIBS)

build/hmi-service-asan$(EXE): $(SRC) src/main.c $(CJSON) src/*.h | build
	$(CC) -O1 -g $(SAN) $(WARN) $(DEFS) -o $@ $(SRC) src/main.c $(CJSON) $(LDLIBS)

build:
	mkdir -p build

# compilation croisée Windows x64 (macOS : brew install mingw-w64 ; Debian/Ubuntu : apt install gcc-mingw-w64-x86-64)
windows:
	mkdir -p build/win
	$(MINGW) -O2 $(WARN) -o build/win/hmi-service.exe $(SRC) src/main.c $(CJSON) -lws2_32 -static
	$(MINGW) -O1 -g $(WARN) -Isrc -o build/win/test_core.exe $(SRC) tests/test_core.c $(CJSON) -lws2_32 -static
	@echo "build/win/hmi-service.exe et build/win/test_core.exe prêts"

unit: $(TESTBIN)
	$(LEAKS) ./$(TESTBIN)

e2e: build/hmi-service-asan$(EXE)
	node tests/e2e.mjs build/hmi-service-asan$(EXE)

test: unit e2e

run: $(BIN)
	./$(BIN) --data data --web web

dist:
	deploy/package.sh

dist-windows: windows
	deploy/package.sh --windows build/win/hmi-service.exe

clean:
	rm -rf build

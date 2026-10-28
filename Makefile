# Audio backend: pipewire | pulse
BACKEND ?= pulse

CC ?= cc
PACKAGE := pavoldcontrol
BUILD_DIR := build
LOCALE_DIR := $(BUILD_DIR)/locale

ifeq ($(BACKEND),pipewire)
BACKEND_PKGS := libpipewire-0.3 libspa-0.2
else ifeq ($(BACKEND),pulse)
BACKEND_PKGS := libpulse libpulse-mainloop-glib
else
$(error Unknown BACKEND '$(BACKEND)', expected 'pipewire' or 'pulse')
endif

PKGS := gtk+-3.0 $(BACKEND_PKGS)

CFLAGS += -Wall -Wextra -pedantic
CFLAGS += $(shell pkg-config --cflags $(PKGS))
CFLAGS += -DGETTEXT_PACKAGE=\"$(PACKAGE)\" -DLOCALEDIR=\"$(abspath $(LOCALE_DIR))\"

LDFLAGS += $(shell pkg-config --libs $(PKGS))
LDLIBS += -lm

SRC := $(wildcard src/*.c) src/backend/$(BACKEND).c
OBJ := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(SRC))
DEP := $(OBJ:.o=.d)
BIN := $(BUILD_DIR)/$(PACKAGE)

# fix GNU warnings on PW/SPA headers
$(BUILD_DIR)/backend/pipewire.o: CFLAGS += -Wno-pedantic

STAMP := $(BUILD_DIR)/.backend

PO_FILES := $(wildcard po/*.po)
MO_FILES := $(patsubst po/%.po,$(LOCALE_DIR)/%/LC_MESSAGES/$(PACKAGE).mo,$(PO_FILES))

.PHONY: all clean locale update-pot create-po update-po FORCE

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/%.o: src/%.c $(STAMP)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(STAMP): FORCE
	@mkdir -p $(BUILD_DIR)
	@echo '$(BACKEND)' | cmp -s - $@ || { echo '$(BACKEND)' > $@; rm -f $(OBJ) $(BIN); }

-include $(DEP)

# Update template
update-pot: src/*.c src/backend/*.c
	xgettext --keyword=_ --keyword=N_ --from-code=UTF-8 \
		--default-domain=$(PACKAGE) \
		--output=po/$(PACKAGE).pot $^

# Create a new .po
create-po:
	msginit --input=po/$(PACKAGE).pot --locale=$(LANG) --output=po/$(LANG).po

# Update existing .po
update-po: $(PO_FILES)
	for po in $(PO_FILES); do \
		msgmerge --update $$po po/$(PACKAGE).pot; \
	done

# Compile .po
$(LOCALE_DIR)/%/LC_MESSAGES/$(PACKAGE).mo: po/%.po
	mkdir -p $(dir $@)
	msgfmt --output-file=$@ $<

# build all locales
locale: $(MO_FILES)

clean:
	rm -rf $(BUILD_DIR)

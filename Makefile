CC ?= cc
PKGS := gtk+-3.0 libpulse libpulse-mainloop-glib

PACKAGE := pavoldcontrol
LOCALE_DIR := $(BUILD_DIR)/locale

CFLAGS += -std=c17 -Wall -Wextra -pedantic
CFLAGS += $(shell pkg-config --cflags $(PKGS))
CFLAGS += -DGETTEXT_PACKAGE=\"$(PACKAGE)\" -DLOCALEDIR=\"$(PWD)/$(LOCALE_DIR)\"

LDFLAGS += $(shell pkg-config --libs $(PKGS))
LDLIBS += -lm

SRC := $(wildcard src/*.c)
BUILD_DIR := build
OBJ := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(SRC))
BIN := $(BUILD_DIR)/$(PACKAGE)

PO_FILES := $(wildcard po/*.po)
MO_FILES := $(patsubst po/%.po,$(LOCALE_DIR)/%/LC_MESSAGES/$(PACKAGE).mo,$(PO_FILES))

.PHONY: all clean locale update-pot create-po update-po

all: $(BIN)

$(BIN): $(OBJ) | $(BUILD_DIR)
	$(CC) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Localization Rules

# Update template
update-pot: src/*.c
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
$(LOCALE_DIR)/%/LC_MESSAGES/$(PACKAGE).mo: po/%.po | $(LOCALE_DIR)
	mkdir -p $(dir $@)
	msgfmt --output-file=$@ $<

# build all locales
locale: $(MO_FILES)

clean:
	rm -rf $(BUILD_DIR)

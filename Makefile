PLUGIN_NAME   := apk

prefix        := $(or $(prefix),$(PREFIX),/usr)
PLUGINDIR     := $(prefix)/lib/collectd

BUILD_DIR     := build

PKG_CONFIG    ?= pkg-config
INSTALL       := install
GIT           := git
SED           := sed

GIT_REV       := $(shell test -d .git && $(GIT) describe --tags --match 'v*' 2>/dev/null)
ifneq ($(GIT_REV),)
  VERSION     := $(patsubst v%,%,$(GIT_REV))
endif

APK_CFLAGS     = $(shell $(PKG_CONFIG) --cflags apk)
APK_LIBS       = $(shell $(PKG_CONFIG) --libs apk)

JSONC_CFLAGS   = $(shell $(PKG_CONFIG) --cflags json-c)
JSONC_LIBS     = $(shell $(PKG_CONFIG) --libs json-c)

COLLECTD_INCLUDE_DIR := /usr/include/collectd/core
COLLECTD_PLUGIN_H     = $(COLLECTD_INCLUDE_DIR)/daemon/plugin.h

ifeq ($(DEBUG), 1)
  CFLAGS      ?= -g -Werror
else
  CFLAGS      ?= -O2 -DNDEBUG
endif

CFLAGS        += -Wall -Wextra -pedantic
CFLAGS        += -std=c11 -fPIC -I$(COLLECTD_INCLUDE_DIR) $(APK_CFLAGS) $(JSONC_CFLAGS)
LDFLAGS       += -shared
LIBS          += $(APK_LIBS) $(JSONC_LIBS)

SRCS           = apk.c
OBJS           = $(SRCS:.c=.o)
TARGET         = $(PLUGIN_NAME).so

D              = $(BUILD_DIR)
MAKEFILE_PATH  = $(lastword $(MAKEFILE_LIST))

all: build

#: Print list of targets.
help:
	@printf '%s\n\n' 'List of targets:'
	@$(SED) -En '/^#:.*/{ N; s/^#: (.*)\n([A-Za-z0-9_-]+).*/\2 \1/p }' $(MAKEFILE_PATH) \
		| while read label desc; do printf '%-15s %s\n' "$$label" "$$desc"; done

.PHONY: help

#: Build sources (the default target).
build: $(D)/$(TARGET)

#: Remove generated files.
clean:
	rm -rf "$(D)"

.PHONY: build clean

#: Install plugin into $DESTDIR/$PLUGINDIR.
install:
	$(INSTALL) -d $(DESTDIR)$(PLUGINDIR)
	$(INSTALL) -m755 $(D)/$(TARGET) $(DESTDIR)$(PLUGINDIR)/

#: Uninstall plugin from $DESTDIR/$PLUGINDIR.
uninstall:
	rm -f "$(DESTDIR)$(PLUGINDIR)/$(TARGET)"

.PHONY: install uninstall

#: Update version in sources and README.adoc to $VERSION.
bump-version:
	test -n "$(VERSION)"  # $$VERSION
	$(SED) -E -i "s/(#define\s+PLUGIN_VERSION\s+).*/\1\"$(VERSION)\"/" $(SRCS)
	$(SED) -E -i "s/^(:version:).*/\1 $(VERSION)/" README.adoc

#: Bump version to $VERSION, create release commit and tag.
release: .check-git-clean | bump-version
	test -n "$(VERSION)"  # $$VERSION
	$(GIT) add .
	$(GIT) commit -m "Release version $(VERSION)"
	$(GIT) tag -s v$(VERSION) -m v$(VERSION)

.PHONY: build-version release

$(D)/%.o: %.c | .builddir $(COLLECTD_PLUGIN_H)
	$(CC) $(CFLAGS) $(if $(VERSION),-DPLUGIN_VERSION='"$(VERSION)"') -o $@ -c $<

$(D)/$(TARGET): $(addprefix $(D)/,$(OBJS))
	$(CC) $(LDFLAGS) -Wl,-soname,$(TARGET) -o $@ $^ $(LIBS)

$(COLLECTD_PLUGIN_H):
	@echo "ERROR: $(COLLECTD_PLUGIN_H) does not exist!" >&2
	@echo "ERROR: Provide COLLECTD_INCLUDE_DIR variable with path to collectd header files directory." >&2
	@false

.builddir:
	@mkdir -p "$(D)"

.check-git-clean:
	@test -z "$(shell $(GIT) status --porcelain)" \
		|| { echo 'You have uncommitted changes!' >&2; exit 1; }

.PHONY: .builddir .check-git-clean

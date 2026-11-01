# Shared LVGL source mirror, included by every N31 app's makefiles.
#
# Compiling LVGL directly from the Windows drive is pathologically slow under
# WSL. Measured on this tree, same compiler, same flags, same twenty files:
#
#     from /mnt/c ............ 267 s
#     from a native mirror ...  14 s
#
# Nineteen times. A full 466-object pass is therefore about an hour and three
# quarters from the mount and about five minutes from a mirror, and the mirror
# itself copies in well under a minute once .git and the build trees are left
# out of it. Every header LVGL includes is read again for every translation
# unit, and each of those reads crosses drvfs.
#
# So the sources are mirrored to the native filesystem once and compiled from
# there. Objects are cached separately, per config - see each makefile.
#
# Staleness is checked, not assumed. A mirror that silently served stale
# sources would be far worse than a slow build, so every invocation asks
# whether any source is newer than the stamp and re-syncs if one is. That is a
# single find over the tree, which costs a second or two even across drvfs, and
# it is the only drvfs traversal left in the build.
#
# Callers must define LVGL (the real tree) before including this, and should
# then use $(LVGL_SRC) everywhere they would have used $(LVGL).

# The tree is mirrored as $(LVGL_MIRROR)/lvgl/ rather than flat, so that the
# include roots keep the shape the real one has: src/lvgl_public.h reaches for
# "../include/lvgl/lvgl.h", and app sources say #include "lvgl/lvgl.h" against
# the mirror's parent. Flattening it breaks both.
LVGL_MIRROR ?= $(HOME)/.cache/lvgl-mirror
LVGL_STAMP  := $(LVGL_MIRROR)/.synced

# Empty when the mirror is present and no source is newer than the stamp.
LVGL_STALE := $(shell \
    if [ ! -f "$(LVGL_STAMP)" ]; then echo stale; \
    elif [ -n "$$(find "$(LVGL)/src" "$(LVGL)/lvgl.h" -newer "$(LVGL_STAMP)" \
                  -print -quit 2>/dev/null)" ]; then echo stale; \
    fi)

ifeq ($(LVGL_STALE),stale)
# Deliberately at parse time rather than as a rule. Every object depends on
# these sources, so the mirror has to exist before make expands a single
# pattern rule against it.
$(info syncing the LVGL mirror to $(LVGL_MIRROR) ...)
$(shell rm -rf "$(LVGL_MIRROR)" && mkdir -p "$(LVGL_MIRROR)/lvgl" && \
        cp -r "$(LVGL)/src" "$(LVGL_MIRROR)/lvgl/" && \
        cp -r "$(LVGL)/include" "$(LVGL_MIRROR)/lvgl/" && \
        cp "$(LVGL)"/*.h "$(LVGL_MIRROR)/lvgl/" && \
        touch "$(LVGL_STAMP)")
endif

LVGL_SRC := $(LVGL_MIRROR)/lvgl

.PHONY: lvgl-resync
lvgl-resync:
	@rm -rf $(LVGL_MIRROR)
	@echo "mirror dropped; the next build will re-sync it"

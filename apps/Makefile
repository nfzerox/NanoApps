# Top-level apps Makefile.
#
# Targets:
#   make all       — build every app
#   make bundles   — build every app + stage as /Apps/<Name>.app/<Name>
#                    bundles in ./out/Apps/
#   make clean     — clean every app + remove staging dir
#
# After `make bundles`, the layout is:
#   apps/out/Apps/
#       Counter.app/Counter   Counter.app/Name.txt
#       Clock.app/Clock       Clock.app/Name.txt
#       ...
# which mirrors what the launcher (apps/launcher) expects on-device.
#
# To deploy in NanoApps:
#   ../tools/install_apps.sh       build + install bundles to /Apps
#   ../tools/run_app.sh paint      build + run one app over SCSI

# Detect every subdir that contains a Makefile. New app == new dir.
# Skip dirs starting with '_' (deploy helpers, not user-facing apps).
ALL_DIRS := $(notdir $(patsubst %/Makefile,%, $(wildcard */Makefile)))
APPS     := $(filter-out _%, $(ALL_DIRS))
HELPERS  := $(filter     _%, $(ALL_DIRS))


# Friendly display name lookup. Overrides + fallback live in ./_name_for.sh.
NAME_FOR := ./_name_for.sh

OUT       := out
BUNDLES   := $(OUT)/Apps

.PHONY: all bundles clean help helpers \
        install \
        hijack-install hijack-uninstall \
        $(APPS) $(HELPERS) $(addprefix run-,$(APPS))

all: $(APPS)

helpers: $(HELPERS)

$(APPS) $(HELPERS):
	@$(MAKE) -s -C $@

# Stage each built .bin into out/Apps/<Name>.app/<Name>
# plus a Name.txt with the friendly display name.
bundles: all helpers
	@rm -rf $(BUNDLES) && mkdir -p $(BUNDLES)
	@for app in $(APPS); do                                              \
	    name=$$($(NAME_FOR) "$$app");                                    \
	    nm_nospace=$$(echo "$$name" | tr -d ' ');                        \
	    bin="$$app/build/$$app.bin";                                     \
	    if [ ! -f "$$bin" ]; then                                        \
	        echo "skip $$app (no $$bin)"; continue;                      \
	    fi;                                                              \
	    dest="$(BUNDLES)/$$nm_nospace.app";                              \
	    mkdir -p "$$dest";                                               \
	    cp "$$bin" "$$dest/$$nm_nospace";                                \
	    printf '%s' "$$name" > "$$dest/Name.txt";                        \
	    echo "  bundle $$nm_nospace.app  <- $$bin";                      \
	done
	@if [ -f _load_stub/build/_load_stub.bin ]; then                     \
	    cp _load_stub/build/_load_stub.bin $(BUNDLES)/.load_stub;        \
	    echo "  staged /Apps/.load_stub (trampoline for app launches)"; \
	fi
	@echo "bundles staged in $(BUNDLES)/"
	@ls -la $(BUNDLES)/

# --- 3 deploy modes ---
#
# MODE 1: `make run-<app>` — single-app SCSI deploy + exec to RAM.
#   Fastest iteration loop for one app. Doesn't touch /Apps on disk.
$(addprefix run-,$(APPS)):
	@app="$(@:run-%=%)"; \
	 echo "building $$app..."; \
	 $(MAKE) -s -C $$app; \
	 echo "deploying $$app via SCSI..."; \
	 ../tools/run_payload.sh 0x0867f8e4 $$app/build/$$app.bin

install:
	../tools/install_apps.sh

# --- Optional: Podcasts -> Homebrew launcher hijack ---
#
# RISKY. Applies a RAM patch to address 0x0844032c.
# Makes tapping Podcasts on the home screen launch /Apps/Homebrew.app/
# Homebrew. Lost on reboot. To revert in-session, run hijack-uninstall.
#
hijack-install: _hijack_install _hijack_stub
	../tools/hijack_podcasts.sh

hijack-uninstall: _hijack_uninstall
	@../tools/run_payload.sh 0x0867f8e4 _hijack_uninstall/build/_hijack_uninstall.bin

clean:
	@for app in $(APPS) $(HELPERS); do $(MAKE) -s -C $$app clean; done
	rm -rf $(OUT)

help:
	@echo "Build:"
	@echo "  all              build every app"
	@echo "  helpers          build deploy helpers (_pusher, ...)"
	@echo "  bundles          build all apps + stage to $(BUNDLES)/"
	@echo "  clean            clean every app + remove $(OUT)/"
	@echo "  <app>            build one app (e.g. 'make clock')"
	@echo ""
	@echo "Deploy modes:"
	@echo "  run-<app>        MODE 1: single-app SCSI exec to RAM"
	@echo "                            (e.g. 'make run-clock')"
	@echo "  install          build + install all app bundles to /Apps"
	@echo ""
	@echo "Optional hijack (opt-in, RAM-only, reboot reverts):"
	@echo "  hijack-install    redirect Podcasts tap -> Homebrew launcher"
	@echo "  hijack-uninstall  revert"
	@echo ""
	@echo "Env:"
	@echo "  SCSI_DEV         iPod disk node (default /dev/sda)"
	@echo "  IPOD_PARTITION   iPod data partition (default /dev/sda2)"

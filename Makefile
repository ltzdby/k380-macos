all: libhidapi.dylib k380 k380-listener

CC=clang

# Vendored hidapi (see hidapi-src/VERSION). Only the macOS backend is needed.
HIDAPI_SRC=hidapi-src/mac/hid.c
HIDAPI_INC=-Ihidapi-src -Ihidapi-src/mac

# Build hidapi as a dylib and link k380 against it dynamically. install_name uses
# @rpath so k380 can locate it via an rpath rather than an absolute path.
libhidapi.dylib: $(HIDAPI_SRC)
	$(CC) -dynamiclib -Wall $(HIDAPI_INC) \
		-install_name @rpath/libhidapi.dylib \
		-framework IOKit -framework CoreFoundation \
		$(HIDAPI_SRC) -o libhidapi.dylib

# k380 resolves libhidapi.dylib at runtime via an rpath of @loader_path, i.e. the
# directory the k380 binary lives in.
k380: k380.o libhidapi.dylib
	$(CC) -Wall k380.o -L. -lhidapi -rpath @loader_path -o k380

k380.o: k380.c
	$(CC) $(HIDAPI_INC) -Wall -c k380.c -o k380.o

# k380-listener is a native XPC-event consumer; it links only system libraries
# and runs the k380 binary, so it has no hidapi dependency.
k380-listener: k380-listener.c
	$(CC) -Wall -fobjc-arc k380-listener.c -o k380-listener

clean:
	rm -f *.o k380 k380-listener libhidapi.dylib

# ---- install / uninstall -----------------------------------------------------

LABEL         = com.local.k380-fnkeys
PLIST_SRC     = com.local.k380-fnkeys.plist.in
LAUNCH_AGENTS = $(HOME)/Library/LaunchAgents
PLIST_DST     = $(LAUNCH_AGENTS)/$(LABEL).plist
K380_BIN      = $(CURDIR)/k380
LISTENER_BIN  = $(CURDIR)/k380-listener

# install renders the plist for this checkout and loads the LaunchAgent. It does
# not run k380 itself: the process that must hold Input Monitoring is the
# launchd-started k380-listener, not the terminal running make. See the printed
# reminders and TROUBLESHOOTING.md for the two required grants.
install: all
	@echo "==> Installing LaunchAgent -> $(PLIST_DST)"
	@mkdir -p "$(LAUNCH_AGENTS)"
	@sed 's|@LISTENER@|$(LISTENER_BIN)|g' "$(PLIST_SRC)" > "$(PLIST_DST)"
	@launchctl bootout gui/$$(id -u)/$(LABEL) 2>/dev/null || true
	@launchctl bootstrap gui/$$(id -u) "$(PLIST_DST)"
	@echo "==> Installed. Logs: ~/Library/Logs/k380-fnkeys.log"
	@echo ""
	@echo "Two grants are required for auto-reapply to work:"
	@echo ""
	@echo "  1. Passwordless sudo for k380 (the listener runs 'sudo -n k380'):"
	@echo "       echo \"$$(id -un) ALL=(ALL) NOPASSWD: $(K380_BIN)\" | sudo tee /etc/sudoers.d/k380"
	@echo "       sudo chmod 440 /etc/sudoers.d/k380"
	@echo ""
	@echo "  2. Input Monitoring for the listener:"
	@echo "       $(LISTENER_BIN)"
	@echo "     macOS may prompt on the next keyboard connect; approve it. If no"
	@echo "     prompt appears, add the listener manually under System Settings ->"
	@echo "     Privacy & Security -> Input Monitoring (the '+' button)."
	@echo ""
	@echo "Check ~/Library/Logs/k380-fnkeys.log for 'rc=0 (F-n keys: ON)'."

# uninstall unloads and removes the LaunchAgent. The sudoers entry is left in
# place (removing it needs root) with a reminder printed.
uninstall:
	@echo "==> Unloading LaunchAgent"
	@launchctl bootout gui/$$(id -u)/$(LABEL) 2>/dev/null || true
	@echo "==> Removing $(PLIST_DST)"
	@rm -f "$(PLIST_DST)"
	@echo "==> Done. To finish cleanup, remove the sudoers entry:"
	@echo "    sudo rm /etc/sudoers.d/k380"

.PHONY: all clean install uninstall


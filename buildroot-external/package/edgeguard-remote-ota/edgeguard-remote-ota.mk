################################################################################
#
# edgeguard-remote-ota
#
################################################################################

EDGEGUARD_REMOTE_OTA_VERSION = 1.0
EDGEGUARD_REMOTE_OTA_SITE = $(BR2_EXTERNAL_EDGEGUARD_PATH)/../RK3588_app/edgeguard-remote-ota-agent
EDGEGUARD_REMOTE_OTA_SITE_METHOD = local
EDGEGUARD_REMOTE_OTA_DEPENDENCIES = host-pkgconf host-python3 libcurl libglib2 json-glib rauc edgeguard-rk-ab
EDGEGUARD_REMOTE_OTA_PACKAGE_DIR = $(BR2_EXTERNAL_EDGEGUARD_PATH)/package/edgeguard-remote-ota
EDGEGUARD_REMOTE_OTA_RELEASE_FILE = $(call qstrip,$(BR2_PACKAGE_EDGEGUARD_REMOTE_OTA_RELEASE_FILE))
EDGEGUARD_REMOTE_OTA_PKG_CONFIG = PKG_CONFIG_SYSROOT_DIR="$(STAGING_DIR)" PKG_CONFIG_LIBDIR="$(STAGING_DIR)/usr/lib/pkgconfig:$(STAGING_DIR)/usr/share/pkgconfig" $(PKG_CONFIG_HOST_BINARY)

define EDGEGUARD_REMOTE_OTA_BUILD_CMDS
	$(HOST_DIR)/bin/python3 $(EDGEGUARD_REMOTE_OTA_PACKAGE_DIR)/prepare-release.py \
		--input "$(EDGEGUARD_REMOTE_OTA_RELEASE_FILE)" --output $(@D)/release.json
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra \
		-I$(@D)/include \
		`$(EDGEGUARD_REMOTE_OTA_PKG_CONFIG) --cflags glib-2.0 json-glib-1.0 libcurl` \
		$(@D)/src/main.c $(@D)/src/config.c $(@D)/src/identity.c \
		$(@D)/src/manifest.c $(@D)/src/version.c $(@D)/src/compatibility.c \
		$(@D)/src/download.c $(@D)/src/persistence.c $(@D)/src/rauc_adapter.c \
		$(@D)/src/state_machine.c $(@D)/src/reporting.c $(@D)/src/time_source.c \
		$(@D)/src/reboot.c $(@D)/src/health.c \
		-o $(@D)/edgeguard-remote-ota-agent $(TARGET_LDFLAGS) \
		`$(EDGEGUARD_REMOTE_OTA_PKG_CONFIG) --libs glib-2.0 json-glib-1.0 libcurl`
endef

define EDGEGUARD_REMOTE_OTA_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/edgeguard-remote-ota-agent \
		$(TARGET_DIR)/usr/bin/edgeguard-remote-ota-agent
	$(INSTALL) -D -m 0644 $(EDGEGUARD_REMOTE_OTA_PACKAGE_DIR)/agent.conf \
		$(TARGET_DIR)/etc/edgeguard-ota/agent.conf
	$(INSTALL) -D -m 0644 $(@D)/release.json \
		$(TARGET_DIR)/etc/edgeguard-ota/release.json
	$(INSTALL) -D -m 0755 \
		$(BR2_EXTERNAL_EDGEGUARD_PATH)/../RK3588_app/edgeguard-rk-ab/edgeguard-rk-ab-preinstall \
		$(TARGET_DIR)/usr/libexec/rauc/edgeguard-rk-ab-preinstall
	$(INSTALL) -D -m 0644 $(EDGEGUARD_REMOTE_OTA_PACKAGE_DIR)/system.conf \
		$(TARGET_DIR)/etc/rauc/system.conf
endef

define EDGEGUARD_REMOTE_OTA_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(EDGEGUARD_REMOTE_OTA_PACKAGE_DIR)/S99edgeguard-remote-ota \
		$(TARGET_DIR)/etc/init.d/S99edgeguard-remote-ota
endef

# Board integration must append post-build-check.sh as its final post-build
# script (README). This runs AFTER rootfs overlays, unlike package install hooks.

$(eval $(generic-package))

################################################################################
#
# edgeguard-provisioning
#
################################################################################

EDGEGUARD_PROVISIONING_VERSION = 1.0
EDGEGUARD_PROVISIONING_SITE = $(BR2_EXTERNAL_EDGEGUARD_PATH)/../RK3588_app/edgeguard-provisioningd
EDGEGUARD_PROVISIONING_SITE_METHOD = local
EDGEGUARD_PROVISIONING_DEPENDENCIES = host-pkgconf libglib2 json-glib
EDGEGUARD_PROVISIONING_PACKAGE_DIR = $(BR2_EXTERNAL_EDGEGUARD_PATH)/package/edgeguard-provisioning
EDGEGUARD_PROVISIONING_PKG_CONFIG = PKG_CONFIG_SYSROOT_DIR="$(STAGING_DIR)" PKG_CONFIG_LIBDIR="$(STAGING_DIR)/usr/lib/pkgconfig:$(STAGING_DIR)/usr/share/pkgconfig" $(PKG_CONFIG_HOST_BINARY)

define EDGEGUARD_PROVISIONING_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra \
		-I$(@D)/include \
		`$(EDGEGUARD_PROVISIONING_PKG_CONFIG) --cflags glib-2.0 gio-2.0 gio-unix-2.0 json-glib-1.0` \
		$(@D)/src/main.c $(@D)/src/store.c $(@D)/src/endpoint.c \
		$(@D)/src/connman.c $(@D)/src/protocol.c $(@D)/src/security.c \
		$(@D)/src/status.c $(@D)/src/operations.c $(@D)/src/input.c \
		$(@D)/src/bluez.c \
		-o $(@D)/edgeguard-provisioningd $(TARGET_LDFLAGS) \
		`$(EDGEGUARD_PROVISIONING_PKG_CONFIG) --libs glib-2.0 gio-2.0 gio-unix-2.0 json-glib-1.0`
endef

define EDGEGUARD_PROVISIONING_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/edgeguard-provisioningd \
		$(TARGET_DIR)/usr/bin/edgeguard-provisioningd
	$(INSTALL) -D -m 0644 $(EDGEGUARD_PROVISIONING_PACKAGE_DIR)/daemon.conf \
		$(TARGET_DIR)/etc/edgeguard-provisioning/daemon.conf
endef

define EDGEGUARD_PROVISIONING_INSTALL_INIT_SYSV
	$(INSTALL) -D -m 0755 $(EDGEGUARD_PROVISIONING_PACKAGE_DIR)/S98edgeguard-provisioning \
		$(TARGET_DIR)/etc/init.d/S98edgeguard-provisioning
endef

$(eval $(generic-package))

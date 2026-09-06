################################################################################
#
# edgeguard-rk-ab
#
################################################################################

EDGEGUARD_RK_AB_VERSION = 1.0
EDGEGUARD_RK_AB_SITE = $(BR2_EXTERNAL_EDGEGUARD_PATH)/../RK3588_app/edgeguard-rk-ab
EDGEGUARD_RK_AB_SITE_METHOD = local

define EDGEGUARD_RK_AB_BUILD_CMDS
	$(TARGET_CC) \
		$(TARGET_CFLAGS) \
		-std=gnu11 \
		-Wall \
		-Wextra \
		$(@D)/edgeguard-rk-abctl.c \
		-o $(@D)/edgeguard-rk-abctl \
		$(TARGET_LDFLAGS)
endef

define EDGEGUARD_RK_AB_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 \
		$(@D)/edgeguard-rk-abctl \
		$(TARGET_DIR)/usr/bin/edgeguard-rk-abctl

	$(INSTALL) -D -m 0755 \
		$(@D)/edgeguard-rk-ab-backend \
		$(TARGET_DIR)/usr/libexec/rauc/edgeguard-rk-ab-backend
endef

$(eval $(generic-package))
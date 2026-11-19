################################################################################
#
# newspaper
#
################################################################################

# The package is this repository itself: the external tree lives in
# targets/linux/image, three levels below the top.
NEWSPAPER_SITE = $(realpath $(BR2_EXTERNAL_NEWSPAPER_PATH)/../../..)
NEWSPAPER_SITE_METHOD = local
NEWSPAPER_SUBDIR = targets/linux
NEWSPAPER_VERSION = $(or $(call qstrip,$(NEWSPAPER_FIRMWARE_VERSION)),$(shell head -n 1 $(NEWSPAPER_SITE)/VERSION))
NEWSPAPER_LICENSE = MIT
NEWSPAPER_LICENSE_FILES = LICENSE
NEWSPAPER_DEPENDENCIES = libcurl
# Keep build output of other targets out of the copy Buildroot makes.
NEWSPAPER_OVERRIDE_SRCDIR_RSYNC_EXCLUSIONS = \
	--exclude .git --exclude build --exclude 'build-*' --exclude node_modules \
	--exclude targets/esp32/build --exclude targets/esp32/managed_components \
	--exclude targets/wasm/dist --exclude targets/wasm/build --exclude output
NEWSPAPER_CONF_OPTS = -DNP_HARDWARE=ON -DNP_STATIC=OFF -DNP_VERSION=$(NEWSPAPER_VERSION)

$(eval $(cmake-package))

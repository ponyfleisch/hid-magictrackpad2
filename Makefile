#!/usr/bin/make -f

PKG_NAME=hid-magictrackpad2-dkms
PKG_VERSION=1.0.0
PKG_DESCRIPTION="Magic Trackpad 2 Support via HID Layer"

MAINTAINER="claudio <linux@ponyfleisch.ch>"
HOMEPAGE="https://github.com/ponyfleisch/hid-magictrackpad2-dkms"

all:
	test -d build || mkdir build
	fpm -f -s dir -t deb -n $(PKG_NAME) -v $(PKG_VERSION) -a all -p build/ \
		-d dkms -d build-essential -d linux-headers-generic \
		-m $(MAINTAINER) --vendor ponyfleisch --license GPLv2 --description $(PKG_DESCRIPTION) \
		--url $(HOMEPAGE) --deb-changelog CHANGELOG \
		--post-install scripts/post-install.sh --pre-uninstall scripts/pre-uninstall.sh \
		--exclude '.git*' usr/

clean:
	rm -f build/$(PKG_NAME)_$(PKG_VERSION)_all.deb

#!/bin/bash

set -e

dkms_name="hid-magictrackpad2"
dkms_version="4.10+hid-magictrackpad2"

case "$1" in
configure)
  # add
  if ! dkms status -m $dkms_name -v $dkms_version | egrep '(added|built|installed)' >/dev/null ; then
    # if it's not been added yet, add it
    dkms add -m $dkms_name -v $dkms_version
  fi

  # build
  if ! dkms status -m $dkms_name -v $dkms_version  | egrep '(built|installed)' >/dev/null ; then
    # if it's not been built yet, build it
    dkms build $dkms_name/$dkms_version
  fi

  # install
  if ! dkms status -m $dkms_name -v $dkms_version  | egrep '(installed)' >/dev/null; then
    # if it's not been installed yet, install it
    dkms install --force $dkms_name/$dkms_version
  fi
;;

*)
  echo "postinst called with unknown argument: $1"
  exit 1
;;
esac

#DEBHELPER#

exit 0

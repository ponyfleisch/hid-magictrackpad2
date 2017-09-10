#!/bin/bash

set -e

dkms_name="hid-magictrackpad2"
dkms_version="4.10+hid-magictrackpad2"

case "$1" in
remove|upgrade|deconfigure)
  if dkms status -m $dkms_name -v $dkms_version | egrep '(added|built|installed)' >/dev/null ; then
    # if dkms bindings exist, remove them
    dkms remove $dkms_name/$dkms_version --all
  fi
;;

*)
  echo "prerm called with unknown argument: $1"
  exit 1
;;
  esac

#DEBHELPER#

exit 0

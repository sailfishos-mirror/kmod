#!/bin/bash
# SPDX-FileCopyrightText: 2024 Emil Velikov <emil.l.velikov@gmail.com>
# SPDX-FileCopyrightText: 2024 Lucas De Marchi <lucas.de.marchi@gmail.com>
#
# SPDX-License-Identifier: LGPL-2.1-or-later

export DEBIAN_FRONTEND=noninteractive
export TZ=Etc/UTC

. /etc/os-release

backports_pkgs=()
if [[ "$VERSION_CODENAME" == "bullseye" ]]; then
    echo "deb http://deb.debian.org/debian bullseye-backports main" >> /etc/apt/sources.list
    backports_pkgs=("meson" "ninja-build")
fi

apt-get update
apt-get install --yes \
    bash \
    build-essential \
    clang \
    gcc-multilib \
    git \
    gtk-doc-tools \
    liblzma-dev \
    libssl-dev \
    libzstd-dev \
    linux-headers-generic \
    meson \
    scdoc \
    zlib1g-dev \
    zstd

if (( ${#backports_pkgs[@]} )); then
    apt-get install --yes -t "${VERSION_CODENAME}"-backports "${backports_pkgs[@]}"
fi

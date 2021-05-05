FROM debian:bullseye

RUN apt-get update -qq && apt-get install --no-install-recommends -y \
    appstream \
    appstream-util \
    automake \
    autotools-dev \
    clang \
    clang-tools-9 \
    cmake \
    dbus \
    desktop-file-utils \
    docbook-xsl \
    gcc \
    g++ \
    gettext \
    git \
    gnome-pkg-tools \
    gobject-introspection \
    gperf \
    gsettings-desktop-schemas-dev \
    gtk-doc-tools \
    itstool \
    lcov \
    libaccountsservice-dev \
    libappstream-dev \
    libappstream-glib-dev \
    libarchive-dev \
    libflatpak-dev \
    libfuse-dev \
    libfwupd-dev \
    libgirepository1.0-dev \
    libglib2.0-dev \
    libgnome-desktop-3-dev \
    libgoa-1.0-dev \
    libgspell-1-dev \
    libgtk-3-dev \
    libgudev-1.0-dev \
    libjson-glib-dev \
    liblmdb-dev \
    libpackagekit-glib2-dev \
    libpam0g-dev \
    libpolkit-gobject-1-dev \
    libsoup2.4-dev \
    librsvg2-dev \
    libstemmer-dev \
    libx11-dev \
    libxmlb-dev \
    libxml2-utils \
    libyaml-dev \
    libz-dev \
    ninja-build \
    packagekit \
    pkg-config \
    policykit-1 \
    python3 \
    python3-pip \
    python3-setuptools \
    python3-wheel \
    shared-mime-info \
    sudo \
    unzip \
    valgrind \
    wget \
    xsltproc \
    xz-utils \
    libtool \
    xxd \
    liblzma-dev \
    gnome-software \
    gnome-software-dev \
    npm \
    valac \
    && rm -rf /usr/share/doc/* /usr/share/man/*

RUN pip3 install meson==0.50.0

# Enable passwordless sudo for sudo users
RUN sed -i -e '/%sudo/s/ALL$/NOPASSWD: ALL/' /etc/sudoers

# Build and install libappimageupdate and libappimage
RUN git clone --recursive https://github.com/AppImage/AppImageUpdate &&\
    mkdir AppImageUpdate/build/ &&\
    cd AppImageUpdate/build/ &&\
    cmake -DBUILD_QT_UI=OFF -DCMAKE_INSTALL_PREFIX=/usr .. &&\
    make -j$(nproc)  &&\
    make install &&\
    cd ../..

RUN git clone --recursive https://github.com/AppImage/AppImageKit &&\
    mkdir AppImageKit/build/ &&\
    cd AppImageKit/build/ &&\
    cmake -DUSE_SYSTEM_XZ=ON -DUSE_SYSTEM_INOTIFY_TOOLS=ON -DUSE_SYSTEM_LIBARCHIVE=ON -DUSE_SYSTEM_GTEST=OFF -DCMAKE_INSTALL_PREFIX=/usr .. &&\
    make -j$(nproc) &&\
    make install


ARG HOST_USER_ID=5555
ENV HOST_USER_ID ${HOST_USER_ID}
RUN useradd -u $HOST_USER_ID -G sudo -ms /bin/bash user

USER user
WORKDIR /home/user

ENV LANG=C.UTF-8 LANGUAGE=C.UTF-8 LC_ALL=C.UTF-8

FROM ubuntu:latest

# grab first dependencies from apt
RUN apt-get update && \
apt-get install -y cmake libuv1 libuv1-dev libjansson4 libjansson-dev \
libzip4 libzip-dev git clang automake autoconf libtool libx264-dev libopus-dev \
yasm libpng-dev libjpeg-turbo8-dev gconf-service libasound2 libatk1.0-0 \
libcairo2 libcups2 libdbus-1-3 libfontconfig1 libfreetype6 libgconf-2-4 \
pkg-config curl libcurl4-gnutls-dev && \
curl -O https://dl.google.com/linux/direct/google-chrome-stable_current_amd64.deb && \
dpkg -i google-chrome-stable_current_amd64.deb; apt-get -fy install && \
curl -sL https://deb.nodesource.com/setup_7.x | bash - && \
apt-get install nodejs && rm -rf /var/lib/apt/lists/*

# Create app directory
RUN mkdir -p /var/lib/ichabod/ext
WORKDIR /var/lib/ichabod/ext

# Build deps: 1 of 2 - ImageMagick

RUN git clone https://github.com/ImageMagick/ImageMagick.git && \
cd ImageMagick && \
git checkout 7.0.4-5 && \
./configure '--with-png=yes' '--with-jpeg=yes' && \
make -j4 && \
make install && \
cd .. && rm -rf ImageMagick

# Build deps: 2 of 2 - FFmpeg

WORKDIR /var/lib/ichabod/ext
RUN git clone https://github.com/FFmpeg/FFmpeg.git && \
cd FFmpeg && \
git checkout n3.2.4 && \
./configure --enable-libx264 --enable-gpl \
  --extra-ldflags=-L/usr/local/lib \
  --extra-cflags=-I/usr/local/include \
  --enable-libopus && \
make -j4 && \
make install && \
cd .. && rm -rf FFmpeg

# Build deps: 3 of 3 - zmq
RUN curl -L https://github.com/zeromq/libzmq/releases/download/v4.2.1/zeromq-4.2.1.tar.gz | tar xz && \
cd zeromq-4.2.1 && ./autogen.sh && ./configure && make -j4 && make install && \
cd .. && rm -rf zeromq-4.2.1

WORKDIR /var/lib/ichabod

# Copy app source
COPY CMakeLists.txt /var/lib/ichabod/CMakeLists.txt
COPY ichabod /var/lib/ichabod/ichabod
# COPY task/index.js /var/lib/barc/task.js
# COPY task/package.json /var/lib/barc/package.json
#
# build barc binary
RUN mkdir -p /var/lib/ichabod/build /var/lib/ichabod/bin && \
cd /var/lib/ichabod/build && \
cmake .. && \
make && mv ichabod ../bin && cd .. && \
rm -rf ichabod build CMakeLists.txt

ENV LD_LIBRARY_PATH=/usr/local/lib
ENV PATH=${PATH}:/var/lib/ichabod/bin
ENV ICHABOD=/var/lib/ichabod/bin/ichabod

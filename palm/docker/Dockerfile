# Palm OS 68k toolchain: prc-tools-remix (m68k-palmos-gcc, build-prc),
# the Palm OS SDKs, and PilRC.
FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
        curl ca-certificates git sudo make xz-utils bzip2 unzip && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/setup
RUN curl -sLO https://github.com/jichu4n/prc-tools-remix/raw/master/tools/setup-palm-os-dev-env.sh \
 && curl -sLO https://github.com/jichu4n/prc-tools-remix/raw/master/tools/install-prc-tools-remix.sh \
 && curl -sLO https://github.com/jichu4n/prc-tools-remix/raw/master/tools/setup-palm-os-sdk.sh \
 && curl -sLO https://github.com/jichu4n/prc-tools-remix/raw/master/tools/install-pilrc.sh \
 && chmod +x *.sh \
 && echo | ./setup-palm-os-dev-env.sh bookworm
WORKDIR /src

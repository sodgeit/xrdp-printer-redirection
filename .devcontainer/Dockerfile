FROM mcr.microsoft.com/devcontainers/base:noble

# Enable deb-src repositories
RUN sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources

# Install xrdp build dependencies + development tools
RUN apt-get update && \
    apt-get build-dep -y xrdp && \
    apt-get install -y --no-install-recommends \
        build-essential \
        devscripts \
        dpkg-dev \
        fakeroot \
        debhelper \
        quilt \
        libcups2-dev \
        cups \
        cups-client \
        ghostscript && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/*

# Fetch xrdp source package (Ubuntu patched)
RUN mkdir -p /usr/src/xrdp && \
    cd /usr/src/xrdp && \
    apt-get update && \
    apt-get source xrdp && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/* && \
    chown -R vscode:vscode /usr/src/xrdp

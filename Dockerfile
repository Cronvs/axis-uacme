ARG ARCH=aarch64
FROM axisecp/acap-computer-vision-sdk:latest-${ARCH}

# Install build dependencies and runtime tools
RUN apt-get update && apt-get install -y \
    gcc \
    make \
    pkg-config \
    libcurl4-openssl-dev \
    libssl-dev \
    uacme \
    ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# Setup App Directory
WORKDIR /app
COPY app /app

# Compile the C application
# We link against libcurl for VAPIX requests
RUN gcc -o acme_daemon main.c -lcurl

# Setup storage
RUN mkdir -p /etc/ssl/uacme/private
RUN chmod +x hook.sh

CMD ["./acme_daemon"]

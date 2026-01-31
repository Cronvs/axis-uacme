ARG ARCH=armv7hf
ARG VERSION=12.8.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

# Copy all local files into the container image
COPY ./app /opt/app/
WORKDIR /opt/app

# Build the .eap application file
# The SDK uses a specific layout; we copy our files to the expected build output
RUN . /opt/axis/acapsdk/environment-setup* && acap-build . -a "lego" -a "update.sh"

# Builder stage
FROM ubuntu:24.04 AS builder

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    xmake \
    curl \
    pkg-config \
    libgflags-dev \
    libglib2.0-dev \
    libqmi-glib-dev \
    libmbim-glib-dev \
    libssl-dev \
    p7zip-full \
    unzip \
    && rm -rf /var/lib/apt/lists/*

# Set the working directory
WORKDIR /app

# Install Xmake dependencies
COPY xmake.lua .
COPY build .

RUN export XMAKE_ROOT=y \
    && xmake f -vD -y

# Copy the source code
COPY . .

# Build the application
RUN export XMAKE_ROOT=y \
    && find /app/build/ -type f -name qmi_sms_reader -exec cp {} /app/build/qmi_sms_reader \;

# Final stage
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    libqmi-glib5 \
    libqmi-proxy \
    libgflags2.2 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Set the working directory
WORKDIR /app

# Copy the built application from the builder stage
COPY --from=builder /app/build/qmi_sms_reader /usr/local/bin/qmi_sms_reader

# Copy configuration file, if needed
COPY config.example.yaml /etc/qmisms/config.yaml

# Set the entrypoint
ENTRYPOINT ["/usr/local/bin/qmi_sms_reader"]

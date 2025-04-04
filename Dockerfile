# Builder stage
FROM ubuntu:20.04 AS builder

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    xmake \
    git \
    pkg-config \
    libglib2.0-dev \
    libqmi-glib-dev \
    libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Set the working directory
WORKDIR /app

# Copy the source code
COPY . .

# Build the application
RUN xmake f -y && xmake -y

# Final stage
FROM ubuntu:20.04

# Set the working directory
WORKDIR /app

# Copy the built application from the builder stage
COPY --from=builder /app/build/qmi_sms_reader /usr/local/bin/qmi_sms_reader

# Copy configuration file, if needed
COPY config.example.yaml /etc/qmisms/config.yaml

# Set the entrypoint
ENTRYPOINT ["/usr/local/bin/qmi_sms_reader"]

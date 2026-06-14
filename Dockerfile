FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc \
        g++ \
        make \
        default-jdk \
        python3 \
        libncurses-dev \
        libconfig-dev \
        libuv1-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY . .

RUN mkdir -p bin && make

EXPOSE 8080

CMD ["bin/server", "server.cfg"]

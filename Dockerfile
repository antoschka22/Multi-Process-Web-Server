FROM ubuntu:22.04 AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential liburing-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY Makefile ./
COPY *.c *.h ./
RUN make

FROM ubuntu:22.04

# Only the io_uring runtime library is needed here (no compiler, no headers).
RUN apt-get update \
    && apt-get install -y --no-install-recommends liburing2 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /build/webserver ./webserver
COPY index.html ./index.html
COPY public ./public

EXPOSE 8080

# Run with:  docker run --security-opt seccomp=unconfined ...
CMD ["./webserver", "8080"]
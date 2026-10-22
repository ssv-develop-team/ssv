# 从固定上游版本构建 MinIO 社区版镜像。
# 上游仓库已归档，此版本仅用于本地/联调，不承诺安全更新。
FROM golang:1.24-bookworm AS build

ARG MINIO_VERSION=RELEASE.2025-04-22T22-12-26Z

ENV CGO_ENABLED=0 \
    GOOS=linux \
    GOPROXY=https://proxy.golang.org,direct

RUN go install github.com/minio/minio@${MINIO_VERSION}

FROM debian:bookworm-slim

ARG MINIO_VERSION=RELEASE.2025-04-22T22-12-26Z

RUN apt-get update \
    && apt-get install -y --no-install-recommends bash ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /go/bin/minio /usr/local/bin/minio

LABEL org.opencontainers.image.source="https://github.com/minio/minio" \
      org.opencontainers.image.version="${MINIO_VERSION}" \
      org.opencontainers.image.licenses="AGPL-3.0"

EXPOSE 9000 9001
VOLUME ["/data"]

ENTRYPOINT ["minio"]
CMD ["server", "/data", "--console-address", ":9001"]

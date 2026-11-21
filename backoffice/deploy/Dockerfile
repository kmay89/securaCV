# One process, one file. `docker compose up -d` from backoffice/.
# Pinned by digest (the index for node:22-slim as resolved from Docker Hub on
# 2026-11-06): a tag moves, the digest is the image that was reviewed. Bump
# both together.
FROM node:26-slim@sha256:e6bedef217677b276c591795e169840edd9096dbf13f29df8ef8d1856b8820bc
WORKDIR /app
COPY package.json ./
COPY bin ./bin
COPY src ./src
# CI stamps the commit and the date in (backoffice-image.yml); /healthz, the Health page and the audit log name them.
ARG BUILD_SHA=dev
ARG BUILD_DATE=
ENV NODE_ENV=production BACKOFFICE_DATA_DIR=/data BACKOFFICE_HOST=0.0.0.0 BACKOFFICE_PORT=8787 BUILD_SHA=$BUILD_SHA BUILD_DATE=$BUILD_DATE
# The volume is created root-owned by the runtime; the process runs as `node`.
RUN mkdir -p /data && chown node:node /data
VOLUME ["/data"]
EXPOSE 8787
USER node
HEALTHCHECK --interval=60s --timeout=5s CMD node -e "fetch('http://127.0.0.1:8787/healthz').then(r=>process.exit(r.ok?0:1)).catch(()=>process.exit(1))"
CMD ["node", "--disable-warning=ExperimentalWarning", "bin/backoffice.mjs", "serve"]

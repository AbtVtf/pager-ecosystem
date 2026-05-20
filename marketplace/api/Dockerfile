# PagerOS marketplace registry — production image.
# Multi-stage build: builder produces a wheel, runtime is a slim image with a
# non-root user and an entrypoint that runs uvicorn against the FastAPI app.

# ----- builder ------------------------------------------------------------- #
FROM python:3.12-slim-bookworm AS builder

ENV PIP_NO_CACHE_DIR=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    PYTHONDONTWRITEBYTECODE=1

WORKDIR /build

RUN pip install --upgrade pip build

COPY pyproject.toml README.md ./
COPY pageros_marketplace ./pageros_marketplace

RUN python -m build --wheel --outdir /wheels


# ----- runtime ------------------------------------------------------------- #
FROM python:3.12-slim-bookworm AS runtime

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    PIP_NO_CACHE_DIR=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    PAGEROS_MKT_HOST=0.0.0.0 \
    PAGEROS_MKT_PORT=8080

# Non-root user.
RUN groupadd --system --gid 1001 pageros \
 && useradd  --system --uid 1001 --gid pageros --home /app --shell /usr/sbin/nologin pageros

WORKDIR /app

COPY --from=builder /wheels /wheels
RUN pip install /wheels/*.whl 'uvicorn[standard]>=0.27,<1' \
 && rm -rf /wheels

USER pageros

EXPOSE 8080

# A startup-period healthcheck keeps the container marked "starting" until
# /healthz responds; production orchestrators can override this if they prefer
# externally driven probes.
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
  CMD python -c "import urllib.request,sys; \
import os; \
url=f'http://127.0.0.1:{os.environ.get(\"PAGEROS_MKT_PORT\",\"8080\")}/healthz'; \
sys.exit(0 if urllib.request.urlopen(url,timeout=2).status==200 else 1)" || exit 1

CMD ["sh", "-c", "exec uvicorn 'pageros_marketplace.registry:create_app' --factory --host ${PAGEROS_MKT_HOST} --port ${PAGEROS_MKT_PORT} --proxy-headers --forwarded-allow-ips=*"]

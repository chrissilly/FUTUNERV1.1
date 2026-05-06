FROM python:3.12-slim

WORKDIR /app

# System deps for sqlite (already present, but explicit)
RUN apt-get update && apt-get install -y --no-install-recommends curl \
    && rm -rf /var/lib/apt/lists/*

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY src ./src

# State directories — persist these as volumes in production
RUN mkdir -p /app/data /app/firmware /app/calibrations

ENV PYTHONUNBUFFERED=1
EXPOSE 8000

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s \
    CMD curl -fsS http://127.0.0.1:8000/health || exit 1

CMD ["uvicorn", "src.main:app", "--host", "0.0.0.0", "--port", "8000"]

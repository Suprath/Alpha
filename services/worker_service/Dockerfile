FROM python:3.11-slim

WORKDIR /app

RUN apt-get update && apt-get install -y libpq-dev gcc && rm -rf /var/lib/apt/lists/*

COPY services/worker_service/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy full repo so absolute imports like `services.worker_service.core` resolve
COPY . .

WORKDIR /app/services/worker_service

ENTRYPOINT ["python", "main.py"]

FROM mcr.microsoft.com/playwright/python:v1.40.0-jammy

WORKDIR /app

COPY services/worker_service/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
RUN pip install --no-cache-dir httpx

COPY . .

ENV PYTHONPATH=/app
ENV PYTHONUNBUFFERED=1

ENTRYPOINT ["python3", "-m", "services.worker_service.main"]

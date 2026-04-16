FROM python:3.12-slim

RUN apt-get update && apt-get install -y \
    gcc \
    libpq-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY services/tui/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY services/tui/ .

# Textual needs a real terminal
ENV TERM=xterm-256color
ENV COLORTERM=truecolor
ENV PYTHONUNBUFFERED=1

CMD ["python", "main.py"]

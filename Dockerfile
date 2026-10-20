FROM python:3.11-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY requirements.lock.txt ./requirements.txt
RUN pip install --no-cache-dir -r requirements.txt
COPY backend ./backend
COPY tools/database_backup.py ./tools/database_backup.py
RUN useradd --uid 10001 --create-home appuser
USER 10001
EXPOSE 8080
CMD ["python","-m","uvicorn","backend.api:create_app","--factory","--host","0.0.0.0","--port","8080","--workers","1","--no-access-log"]

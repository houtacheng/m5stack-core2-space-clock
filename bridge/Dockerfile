FROM python:3.12-alpine
RUN pip install --no-cache-dir websockets==13.1
COPY bridge.py /app/bridge.py
CMD ["python", "-u", "/app/bridge.py"]

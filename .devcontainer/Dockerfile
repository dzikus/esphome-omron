FROM mcr.microsoft.com/devcontainers/python:3.15@sha256:4e9aee4b9498b21ecb00e3049ea050ae90d2dbe735ccac7f7f796f0ddc03d9fb

USER root
COPY requirements.txt /tmp/requirements.txt
RUN grep -v '^homeassistant' /tmp/requirements.txt > /tmp/tools.txt \
    && pip install --no-cache-dir -r /tmp/tools.txt \
    && pip install --no-cache-dir --no-deps "$(grep '^homeassistant' /tmp/requirements.txt)" \
    && rm /tmp/requirements.txt /tmp/tools.txt
USER vscode
RUN mkdir -p /home/vscode/.cache/esphome /home/vscode/.platformio

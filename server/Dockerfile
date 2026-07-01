FROM node:20-bookworm-slim
WORKDIR /app
RUN apt-get update \
  && apt-get install -y --no-install-recommends python3 python3-venv \
  && rm -rf /var/lib/apt/lists/*
COPY requirements-rune.txt ./
RUN python3 -m venv /opt/rune-venv \
  && /opt/rune-venv/bin/pip install --no-cache-dir -r requirements-rune.txt
ENV PATH="/opt/rune-venv/bin:${PATH}"
COPY package*.json ./
RUN npm install --omit=dev
COPY . .
EXPOSE 8080
CMD ["node", "server.js"]
